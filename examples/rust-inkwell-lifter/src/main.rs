use std::collections::BTreeMap;
use std::ffi::{CStr, CString, c_char, c_void};
use std::path::Path;
use std::ptr;
use std::slice;

use inkwell::AddressSpace;
use inkwell::values::{AsValueRef, BasicMetadataValueEnum, PointerValue};

#[cxx::bridge(namespace = "revng_inkwell")]
mod ffi {
    unsafe extern "C++" {
        include!("revng-rust-inkwell-lifter-example/cxx/include/bridge.h");

        unsafe fn tag_root(function: usize);
        unsafe fn tag_marker(function: usize);
        unsafe fn tag_csv(global: usize);
        unsafe fn basic_block_id(module: usize, address: u64) -> usize;
        fn code_x86_64_type() -> u16;
        unsafe fn set_block_type(terminator: usize, kind: u8);
    }
}

#[derive(Clone, Copy, Debug)]
struct DecodedInstruction {
    address: u64,
    kind: u8,
    size: u8,
}

fn parse_address(value: &str) -> Result<u64, String> {
    let address = value
        .split_once(':')
        .map_or(value, |(address, _)| address)
        .strip_prefix("0x")
        .ok_or_else(|| format!("invalid revng address: {value}"))?;
    u64::from_str_radix(address, 16).map_err(|error| error.to_string())
}

fn decode_x86_64(binary: &[u8], entry: u64) -> Result<Vec<DecodedInstruction>, String> {
    let mut decoded = Vec::new();
    let mut offset = 0;
    while offset < binary.len() {
        let (kind, size) = match binary[offset..] {
            [0x90, ..] => (0, 1),       // NOP
            [0xc3, ..] => (1, 1),       // RET
            [0x89, 0xf8, ..] => (2, 2), // MOV EAX, EDI
            [0x01, 0xf0, ..] => (3, 2), // ADD EAX, ESI
            _ => {
                return Err(format!(
                    "unsupported opcode 0x{:02x} at offset {offset}",
                    binary[offset]
                ));
            }
        };
        decoded.push(DecodedInstruction {
            address: entry + offset as u64,
            kind,
            size,
        });
        if kind == 1 {
            return Ok(decoded);
        }
        offset += usize::from(size);
    }
    Err("the example input has no RET".into())
}

struct CallbackContext {
    bytes: [u8; 5],
    error: CString,
}

unsafe extern "C" fn architecture(_: *mut c_void) -> *const c_char {
    c"x86_64".as_ptr()
}

unsafe extern "C" fn entry_point(_: *mut c_void) -> *const c_char {
    c"0x400000:Code_x86_64".as_ptr()
}

unsafe extern "C" fn mapping_count(_: *mut c_void) -> u64 {
    1
}

unsafe extern "C" fn mapping_at(
    opaque: *mut c_void,
    index: u64,
    output: *mut revng_sys::rp_address_space_mapping,
) -> bool {
    if index != 0 || output.is_null() {
        return false;
    }
    let context = unsafe { &mut *opaque.cast::<CallbackContext>() };
    unsafe {
        *output = revng_sys::rp_address_space_mapping {
            start: c"0x400000:Code_x86_64".as_ptr(),
            virtual_size: context.bytes.len() as u64,
            backing_size: context.bytes.len() as u64,
            readable: true,
            writeable: false,
            executable: true,
            name: c"rust-inkwell-example-code".as_ptr(),
        };
    }
    true
}

unsafe extern "C" fn read_bytes(
    opaque: *mut c_void,
    mapping_index: u64,
    offset: u64,
    destination: *mut u8,
    size: u64,
) -> bool {
    let context = unsafe { &mut *opaque.cast::<CallbackContext>() };
    let Ok(offset) = usize::try_from(offset) else {
        return false;
    };
    let Ok(size) = usize::try_from(size) else {
        return false;
    };
    if mapping_index != 0
        || destination.is_null()
        || offset > context.bytes.len()
        || size > context.bytes.len() - offset
    {
        return false;
    }
    unsafe {
        ptr::copy_nonoverlapping(context.bytes.as_ptr().add(offset), destination, size);
    }
    true
}

unsafe extern "C" fn extra_code_address_count(_: *mut c_void) -> u64 {
    0
}

unsafe extern "C" fn lift_callback(
    opaque: *mut c_void,
    _model: *const c_void,
    binary: *const revng_sys::rp_binary_view,
    entries: *const *const c_char,
    entry_count: u64,
    output: revng_sys::LLVMModuleRef,
    error_message: *mut *const c_char,
) -> bool {
    let context = unsafe { &mut *opaque.cast::<CallbackContext>() };
    let entry_pointers = if entry_count == 0 {
        &[][..]
    } else {
        unsafe { slice::from_raw_parts(entries, entry_count as usize) }
    };
    let rust_entries: Vec<String> = entry_pointers
        .iter()
        .map(|entry| {
            unsafe { CStr::from_ptr(*entry) }
                .to_string_lossy()
                .into_owned()
        })
        .collect();
    let entry = rust_entries
        .first()
        .map(String::as_str)
        .unwrap_or("0x400000:Code_x86_64");
    let mut bytes = [0_u8; 5];
    if !unsafe {
        revng_sys::rp_binary_view_read_address(
            binary,
            CString::new(entry).unwrap().as_ptr(),
            bytes.len() as u64,
            bytes.as_mut_ptr(),
            ptr::null_mut(),
        )
    } {
        context.error = CString::new("failed to lazily read the example function").unwrap();
        if !error_message.is_null() {
            unsafe { *error_message = context.error.as_ptr() };
        }
        return false;
    }
    let result = lift(&bytes, entry, output as usize);
    if result.is_empty() {
        return true;
    }
    context.error = CString::new(result.replace('\0', "\\0")).unwrap();
    if !error_message.is_null() {
        unsafe { *error_message = context.error.as_ptr() };
    }
    false
}

fn raw_value<T: AsValueRef>(value: T) -> usize {
    value.as_value_ref() as usize
}

fn emit_with_inkwell(
    output: usize,
    instructions: &[DecodedInstruction],
    entry: u64,
) -> Result<(), String> {
    if output == 0 {
        return Err("null LLVM module".into());
    }
    if instructions.is_empty() {
        return Err("no decoded instructions".into());
    }

    let module = unsafe { revng_inkwell::BorrowedModule::from_raw(output as _) };
    let context = module.get_context();
    let builder = context.create_builder();
    let i64_type = context.i64_type();
    let i32_type = context.i32_type();
    let i16_type = context.i16_type();
    let pointer_type = context.ptr_type(AddressSpace::default());

    let make_csv = |name: &str| {
        let global = module.add_global(i64_type, None, name);
        global.set_initializer(&i64_type.const_zero());
        unsafe { ffi::tag_csv(raw_value(global)) };
        global
    };
    let sp = make_csv("_rsp");
    let pc = make_csv("_rip");
    let rax = make_csv("_rax");
    let rdi = make_csv("_rdi");
    let rsi = make_csv("_rsi");
    let pc_epoch = module.add_global(i32_type, None, "pc_epoch");
    pc_epoch.set_initializer(&i32_type.const_zero());
    let pc_address_space = module.add_global(i16_type, None, "pc_address_space");
    pc_address_space.set_initializer(&i16_type.const_zero());
    let pc_type = module.add_global(i16_type, None, "pc_type");
    pc_type.set_initializer(&i16_type.const_zero());

    let root_type = context.void_type().fn_type(&[i64_type.into()], false);
    let root = module.add_function("root", root_type, None);
    unsafe { ffi::tag_root(raw_value(root)) };

    let newpc_type = context.void_type().fn_type(
        &[
            pointer_type.into(),
            i64_type.into(),
            i32_type.into(),
            i32_type.into(),
            pointer_type.into(),
        ],
        true,
    );
    let newpc = module.add_function("newpc", newpc_type, None);
    unsafe { ffi::tag_marker(raw_value(newpc)) };

    let entry_block = context.append_basic_block(root, "entrypoint");
    let dispatcher = context.append_basic_block(root, "dispatcher");
    let dispatcher_default = context.append_basic_block(root, "dispatcher.default");
    let anypc = context.append_basic_block(root, "anypc");
    let unexpectedpc = context.append_basic_block(root, "unexpectedpc");

    builder.position_at_end(entry_block);
    builder
        .build_store(sp.as_pointer_value(), root.get_first_param().unwrap())
        .map_err(|error| error.to_string())?;
    builder
        .build_store(pc.as_pointer_value(), i64_type.const_int(entry, false))
        .map_err(|error| error.to_string())?;
    builder
        .build_store(pc_epoch.as_pointer_value(), i32_type.const_zero())
        .map_err(|error| error.to_string())?;
    builder
        .build_store(pc_address_space.as_pointer_value(), i16_type.const_zero())
        .map_err(|error| error.to_string())?;
    builder
        .build_store(
            pc_type.as_pointer_value(),
            i16_type.const_int(u64::from(ffi::code_x86_64_type()), false),
        )
        .map_err(|error| error.to_string())?;
    builder
        .build_unconditional_branch(dispatcher)
        .map_err(|error| error.to_string())?;

    let mut blocks = BTreeMap::new();
    for instruction in instructions {
        let block =
            context.append_basic_block(root, &format!("instruction_{:x}", instruction.address));
        blocks.insert(instruction.address, block);
    }

    builder.position_at_end(dispatcher);
    let current_pc = builder
        .build_load(i64_type, pc.as_pointer_value(), "current_pc")
        .map_err(|error| error.to_string())?
        .into_int_value();
    let cases: Vec<_> = instructions
        .iter()
        .map(|instruction| {
            (
                i64_type.const_int(instruction.address, false),
                blocks[&instruction.address],
            )
        })
        .collect();
    let dispatch = builder
        .build_switch(current_pc, dispatcher_default, &cases)
        .map_err(|error| error.to_string())?;

    for (index, instruction) in instructions.iter().enumerate() {
        builder.position_at_end(blocks[&instruction.address]);
        let block_id = unsafe { ffi::basic_block_id(output, instruction.address) };
        let block_id = unsafe { PointerValue::new(block_id as _) };
        let null = pointer_type.const_null();
        let arguments: [BasicMetadataValueEnum; 5] = [
            block_id.into(),
            i64_type
                .const_int(u64::from(instruction.size), false)
                .into(),
            i32_type.const_int(u64::from(index == 0), false).into(),
            i32_type.const_zero().into(),
            null.into(),
        ];
        builder
            .build_call(newpc, &arguments, "")
            .map_err(|error| error.to_string())?;

        match instruction.kind {
            0 | 2 | 3 => {
                match instruction.kind {
                    0 => {}
                    2 => {
                        let source = builder
                            .build_load(i64_type, rdi.as_pointer_value(), "argument_a")
                            .map_err(|error| error.to_string())?
                            .into_int_value();
                        let source = builder
                            .build_int_truncate(source, i32_type, "argument_a_i32")
                            .map_err(|error| error.to_string())?;
                        let result = builder
                            .build_int_z_extend(source, i64_type, "result_i64")
                            .map_err(|error| error.to_string())?;
                        builder
                            .build_store(rax.as_pointer_value(), result)
                            .map_err(|error| error.to_string())?;
                    }
                    3 => {
                        let lhs = builder
                            .build_load(i64_type, rax.as_pointer_value(), "lhs")
                            .map_err(|error| error.to_string())?
                            .into_int_value();
                        let rhs = builder
                            .build_load(i64_type, rsi.as_pointer_value(), "argument_b")
                            .map_err(|error| error.to_string())?
                            .into_int_value();
                        let lhs = builder
                            .build_int_truncate(lhs, i32_type, "lhs_i32")
                            .map_err(|error| error.to_string())?;
                        let rhs = builder
                            .build_int_truncate(rhs, i32_type, "argument_b_i32")
                            .map_err(|error| error.to_string())?;
                        let sum = builder
                            .build_int_add(lhs, rhs, "sum")
                            .map_err(|error| error.to_string())?;
                        let result = builder
                            .build_int_z_extend(sum, i64_type, "sum_i64")
                            .map_err(|error| error.to_string())?;
                        builder
                            .build_store(rax.as_pointer_value(), result)
                            .map_err(|error| error.to_string())?;
                    }
                    _ => unreachable!(),
                }
                let next = instructions
                    .get(index + 1)
                    .ok_or_else(|| "NOP is missing a successor".to_owned())?;
                builder
                    .build_store(
                        pc.as_pointer_value(),
                        i64_type.const_int(next.address, false),
                    )
                    .map_err(|error| error.to_string())?;
                builder
                    .build_unconditional_branch(blocks[&next.address])
                    .map_err(|error| error.to_string())?;
            }
            1 => {
                let indirect_exit = context
                    .append_basic_block(root, &format!("indirect_exit_{:x}", instruction.address));
                builder
                    .build_unconditional_branch(indirect_exit)
                    .map_err(|error| error.to_string())?;
                builder.position_at_end(indirect_exit);
                let stack_pointer = builder
                    .build_load(i64_type, sp.as_pointer_value(), "stack_pointer")
                    .map_err(|error| error.to_string())?
                    .into_int_value();
                let stack_pointer_as_pointer = builder
                    .build_int_to_ptr(stack_pointer, pointer_type, "stack_pointer_as_pointer")
                    .map_err(|error| error.to_string())?;
                let return_address = builder
                    .build_load(i64_type, stack_pointer_as_pointer, "return_address")
                    .map_err(|error| error.to_string())?;
                builder
                    .build_store(pc.as_pointer_value(), return_address)
                    .map_err(|error| error.to_string())?;
                let next_sp = builder
                    .build_int_add(stack_pointer, i64_type.const_int(8, false), "next_sp")
                    .map_err(|error| error.to_string())?;
                builder
                    .build_store(sp.as_pointer_value(), next_sp)
                    .map_err(|error| error.to_string())?;
                builder
                    .build_unconditional_branch(anypc)
                    .map_err(|error| error.to_string())?;
            }
            _ => return Err("unknown decoded instruction kind".into()),
        }
    }

    builder.position_at_end(anypc);
    let anypc_terminator = builder
        .build_unconditional_branch(dispatcher)
        .map_err(|error| error.to_string())?;
    unsafe { ffi::set_block_type(raw_value(anypc_terminator), 2) };
    builder.position_at_end(unexpectedpc);
    let unexpected_terminator = builder
        .build_unconditional_branch(dispatcher)
        .map_err(|error| error.to_string())?;
    unsafe { ffi::set_block_type(raw_value(unexpected_terminator), 3) };
    builder.position_at_end(dispatcher_default);
    let failure_terminator = builder
        .build_unreachable()
        .map_err(|error| error.to_string())?;
    unsafe { ffi::set_block_type(raw_value(failure_terminator), 1) };
    unsafe { ffi::set_block_type(raw_value(dispatch), 0) };

    module.verify().map_err(|error| error.to_string())
}

fn lift(binary: &[u8], entry_text: &str, output: usize) -> String {
    let entry = match parse_address(entry_text) {
        Ok(value) => value,
        Err(error) => return error,
    };

    let decoded = match decode_x86_64(binary, entry) {
        Ok(value) => value,
        Err(error) => return error,
    };

    emit_with_inkwell(output, &decoded, entry)
        .err()
        .unwrap_or_default()
}

unsafe fn pipeline_error(error: *mut revng_sys::rp_error, fallback: &str) -> String {
    if !error.is_null() {
        let simple = unsafe { revng_sys::rp_error_get_simple_error(error) };
        if !simple.is_null() {
            let message = unsafe { revng_sys::rp_simple_error_get_message(simple) };
            if !message.is_null() {
                return unsafe { CStr::from_ptr(message) }
                    .to_string_lossy()
                    .into_owned();
            }
        }
        let document = unsafe { revng_sys::rp_error_get_document_error(error) };
        if !document.is_null()
            && unsafe { revng_sys::rp_document_error_reasons_count(document) } != 0
        {
            let message = unsafe { revng_sys::rp_document_error_get_error_message(document, 0) };
            if !message.is_null() {
                return unsafe { CStr::from_ptr(message) }
                    .to_string_lossy()
                    .into_owned();
            }
        }
    }
    fallback.into()
}

#[derive(Clone, Copy)]
enum Artifact {
    LiftedModule,
    DecompiledC,
}

unsafe fn take_buffer(buffer: *mut revng_sys::rp_buffer) -> Vec<u8> {
    let size = unsafe { revng_sys::rp_buffer_size(buffer) };
    let data = unsafe { revng_sys::rp_buffer_data(buffer) };
    let bytes = if size == 0 || data.is_null() {
        Vec::new()
    } else {
        unsafe { slice::from_raw_parts(data.cast(), size as usize) }.to_vec()
    };
    unsafe { revng_sys::rp_buffer_destroy(buffer) };
    bytes
}

unsafe fn run_initialized(backend_name: &str, artifact: Artifact) -> Result<Vec<u8>, String> {
    let mut context = CallbackContext {
        // int32_t add(int32_t a, int32_t b) { return a + b; }
        // mov eax, edi; add eax, esi; ret
        bytes: [0x89, 0xf8, 0x01, 0xf0, 0xc3],
        error: CString::new("").unwrap(),
    };
    let callbacks = revng_sys::rp_address_space_callbacks {
        opaque: ptr::from_mut(&mut context).cast(),
        architecture: Some(architecture),
        entry_point: Some(entry_point),
        mapping_count: Some(mapping_count),
        mapping_at: Some(mapping_at),
        read: Some(read_bytes),
        extra_code_address_count: Some(extra_code_address_count),
        extra_code_address_at: None,
        release: None,
    };
    let error = unsafe { revng_sys::rp_error_create() };
    if error.is_null() {
        return Err("rp_error_create failed".into());
    }
    let manager = unsafe {
        revng_sys::rp_manager_create_from_address_space(
            &callbacks,
            0,
            0,
            ptr::null(),
            c"".as_ptr(),
            error,
        )
    };
    if manager.is_null() {
        let result = unsafe { pipeline_error(error, "failed to create revng manager") };
        unsafe { revng_sys::rp_error_destroy(error) };
        return Err(result);
    }

    let int32 = revng_sys::rp_primitive_type {
        kind: revng_sys::rp_primitive_kind::RP_PRIMITIVE_KIND_SIGNED,
        size: 4,
    };
    let arguments = [
        revng_sys::rp_cabi_argument {
            name: c"a".as_ptr(),
            type_: int32,
        },
        revng_sys::rp_cabi_argument {
            name: c"b".as_ptr(),
            type_: int32,
        },
    ];
    if !unsafe {
        revng_sys::rp_manager_set_cabi_prototype(
            manager,
            c"0x400000:Code_x86_64".as_ptr(),
            c"SystemV_x86_64".as_ptr(),
            c"add".as_ptr(),
            arguments.len() as u64,
            arguments.as_ptr(),
            &int32,
            error,
        )
    } {
        let result = unsafe { pipeline_error(error, "failed to set the add prototype") };
        unsafe {
            revng_sys::rp_manager_destroy(manager);
            revng_sys::rp_error_destroy(error);
        }
        return Err(result);
    }

    let selected = if backend_name == "inkwell" {
        let lifter = revng_sys::rp_lifter_callbacks {
            opaque: ptr::from_mut(&mut context).cast(),
            lift: Some(lift_callback),
        };
        unsafe { revng_sys::rp_set_lifter(manager, &lifter, error) }
    } else {
        let name = CString::new(backend_name).unwrap();
        unsafe { revng_sys::rp_manager_set_lifter_backend(manager, name.as_ptr(), error) }
    };
    if !selected {
        let result = unsafe { pipeline_error(error, "failed to select lifter backend") };
        unsafe {
            revng_sys::rp_manager_destroy(manager);
            revng_sys::rp_error_destroy(error);
        }
        return Err(result);
    }

    // Materialize the lifted module, run ordinary Inkwell LLVM passes on the
    // borrowed transaction, and commit it before requesting later artifacts.
    let lifted = unsafe {
        revng_sys::rp_manager_produce_artifact(
            manager,
            c"lift".as_ptr(),
            c"root.bc.zstd".as_ptr(),
            c"root".as_ptr(),
            0,
            ptr::null(),
            error,
        )
    };
    if lifted.is_null() {
        let result = unsafe { pipeline_error(error, "lifting failed") };
        unsafe {
            revng_sys::rp_manager_destroy(manager);
            revng_sys::rp_error_destroy(error);
        }
        return Err(result);
    }
    unsafe { revng_sys::rp_buffer_destroy(lifted) };

    let transformed = unsafe {
        revng_inkwell::transform_llvm_module(manager, c"lift", c"root.bc.zstd", error, |module| {
            module.run_passes("instcombine,reassociate")
        })
    };
    if !transformed {
        let result = unsafe { pipeline_error(error, "Inkwell transform failed") };
        unsafe {
            revng_sys::rp_manager_destroy(manager);
            revng_sys::rp_error_destroy(error);
        }
        return Err(result);
    }

    let result = if matches!(artifact, Artifact::DecompiledC) {
        let output = unsafe { revng_sys::rp_manager_decompile_to_c(manager, error) };
        if output.is_null() {
            Err(unsafe { pipeline_error(error, "C decompilation failed") })
        } else {
            Ok(unsafe { take_buffer(output) })
        }
    } else {
        let output = unsafe {
            revng_sys::rp_manager_produce_artifact(
                manager,
                c"lift".as_ptr(),
                c"root.bc.zstd".as_ptr(),
                c"root".as_ptr(),
                0,
                ptr::null(),
                error,
            )
        };
        if output.is_null() {
            Err(unsafe { pipeline_error(error, "artifact production failed") })
        } else {
            let bytes = unsafe { take_buffer(output) };
            if bytes.is_empty() {
                Err("revng produced an empty artifact".into())
            } else {
                Ok(bytes)
            }
        }
    };
    unsafe {
        revng_sys::rp_manager_destroy(manager);
        revng_sys::rp_error_destroy(error);
    }
    result
}

fn run_artifact(
    backend_name: &str,
    pipeline_path: &str,
    artifact: Artifact,
) -> Result<Vec<u8>, String> {
    let pipeline_option = CString::new(format!("--pipeline-path={}", pipeline_path)).unwrap();
    let program = CString::new("revng-rust-inkwell-lifter-example").unwrap();
    let arguments = [program.as_ptr(), pipeline_option.as_ptr()];
    if !unsafe { revng_sys::rp_initialize(2, arguments.as_ptr(), 0, ptr::null_mut()) } {
        return Err("rp_initialize failed".into());
    }
    let result = unsafe { run_initialized(backend_name, artifact) };
    if !unsafe { revng_sys::rp_shutdown() } && result.is_ok() {
        return Err("rp_shutdown failed".into());
    }
    result
}

fn run(backend_name: &str) -> Result<usize, String> {
    run_artifact(
        backend_name,
        env!("REVNG_EXAMPLE_PIPELINE"),
        Artifact::LiftedModule,
    )
    .map(|output| output.len())
}

pub fn decompile_main() {
    let mut arguments = std::env::args().skip(1);
    let backend_name = arguments.next().unwrap_or_else(|| "inkwell".to_owned());
    let output = arguments
        .next()
        .unwrap_or_else(|| "decompiled.c".to_owned());
    if arguments.next().is_some()
        || !matches!(backend_name.as_str(), "inkwell" | "reference-x86_64")
    {
        eprintln!("usage: revng-rust-decompile-example [inkwell|reference-x86_64] [output.c]");
        std::process::exit(2);
    }

    match run_artifact(
        &backend_name,
        env!("REVNG_FULL_PIPELINE"),
        Artifact::DecompiledC,
    ) {
        Ok(contents) => {
            if let Err(error) = std::fs::write(Path::new(&output), &contents) {
                eprintln!("failed to write {output}: {error}");
                std::process::exit(1);
            }
            println!(
                "{backend_name} produced {} bytes of decompiled C in {output}",
                contents.len()
            );
        }
        Err(error) => {
            eprintln!("{error}");
            std::process::exit(1);
        }
    }
}

fn main() {
    let backend_name = std::env::args()
        .nth(1)
        .unwrap_or_else(|| "inkwell".to_owned());
    if !matches!(
        backend_name.as_str(),
        "inkwell" | "reference-x86_64" | "libtcg"
    ) {
        eprintln!("usage: revng-rust-inkwell-lifter-example [inkwell|reference-x86_64|libtcg]");
        std::process::exit(2);
    }

    match run(&backend_name) {
        Ok(size) => println!("{backend_name} produced {size} bytes of LLVM module"),
        Err(error) => {
            eprintln!("{error}");
            std::process::exit(1);
        }
    }
}

#[cfg(test)]
mod test {
    use super::{decode_x86_64, parse_address};

    #[test]
    fn parses_revng_meta_address() {
        assert_eq!(parse_address("0x400000:Code_x86_64").unwrap(), 0x400000);
    }

    #[test]
    fn decodes_nop_ret_and_stops_at_return() {
        let decoded = decode_x86_64(&[0x90, 0xc3, 0xcc], 0x400000).unwrap();
        assert_eq!(decoded.len(), 2);
        assert_eq!(decoded[0].address, 0x400000);
        assert_eq!(decoded[0].kind, 0);
        assert_eq!(decoded[1].address, 0x400001);
        assert_eq!(decoded[1].kind, 1);
    }

    #[test]
    fn decodes_add_function() {
        let decoded = decode_x86_64(&[0x89, 0xf8, 0x01, 0xf0, 0xc3], 0x400000).unwrap();
        assert_eq!(decoded.len(), 3);
        assert_eq!(
            (decoded[0].address, decoded[0].kind, decoded[0].size),
            (0x400000, 2, 2)
        );
        assert_eq!(
            (decoded[1].address, decoded[1].kind, decoded[1].size),
            (0x400002, 3, 2)
        );
        assert_eq!(
            (decoded[2].address, decoded[2].kind, decoded[2].size),
            (0x400004, 1, 1)
        );
    }

    #[test]
    fn rejects_unsupported_or_unterminated_input() {
        assert!(
            decode_x86_64(&[0xcc], 0x400000)
                .unwrap_err()
                .contains("unsupported opcode")
        );
        assert!(
            decode_x86_64(&[0x90], 0x400000)
                .unwrap_err()
                .contains("no RET")
        );
    }
}
