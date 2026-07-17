use std::collections::BTreeMap;
use std::ffi::{c_char, c_void, CStr, CString};
use std::mem::ManuallyDrop;
use std::ptr;
use std::slice;

use inkwell::module::Module;
use inkwell::values::{AsValueRef, BasicMetadataValueEnum, PointerValue};
use inkwell::AddressSpace;

#[cxx::bridge(namespace = "revng_inkwell")]
mod ffi {
    unsafe extern "C++" {
        include!("include/bridge.h");

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
}

fn parse_address(value: &str) -> Result<u64, String> {
    let address = value
        .split_once(':')
        .map_or(value, |(address, _)| address)
        .strip_prefix("0x")
        .ok_or_else(|| format!("invalid revng address: {value}"))?;
    u64::from_str_radix(address, 16).map_err(|error| error.to_string())
}

fn model_entry(model_yaml: &str) -> Option<&str> {
    let suffix = ":Code_x86_64";
    let end = model_yaml.find(suffix)? + suffix.len();
    let start = model_yaml[..end].rfind("0x")?;
    Some(&model_yaml[start..end])
}

fn decode_x86_64(binary: &[u8], entry: u64) -> Result<Vec<DecodedInstruction>, String> {
    let mut decoded = Vec::new();
    for (offset, opcode) in binary.iter().copied().enumerate() {
        let kind = match opcode {
            0x90 => 0,
            0xc3 => 1,
            _ => {
                return Err(format!(
                    "unsupported opcode 0x{opcode:02x} at offset {offset}"
                ))
            }
        };
        decoded.push(DecodedInstruction {
            address: entry + offset as u64,
            kind,
        });
        if kind == 1 {
            return Ok(decoded);
        }
    }
    Err("the example input has no RET".into())
}

struct CallbackContext {
    bytes: [u8; 2],
    error: CString,
}

unsafe extern "C" fn architecture(_: *mut c_void) -> *const c_char {
    b"x86_64\0".as_ptr().cast()
}

unsafe extern "C" fn entry_point(_: *mut c_void) -> *const c_char {
    b"0x400000:Code_x86_64\0".as_ptr().cast()
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
            start: b"0x400000:Code_x86_64\0".as_ptr().cast(),
            virtual_size: 4,
            contents: context.bytes.as_ptr(),
            contents_size: context.bytes.len() as u64,
            readable: true,
            writeable: false,
            executable: true,
            name: b"rust-inkwell-example-code\0".as_ptr().cast(),
        };
    }
    true
}

unsafe extern "C" fn extra_code_address_count(_: *mut c_void) -> u64 {
    0
}

unsafe extern "C" fn lift_callback(
    opaque: *mut c_void,
    model_yaml: *const c_char,
    binary: *const u8,
    binary_size: u64,
    entries: *const *const c_char,
    entry_count: u64,
    output: revng_sys::LLVMModuleRef,
    error_message: *mut *const c_char,
) -> bool {
    let context = unsafe { &mut *opaque.cast::<CallbackContext>() };
    let model_yaml = unsafe { CStr::from_ptr(model_yaml) }.to_string_lossy();
    let binary = unsafe { slice::from_raw_parts(binary, binary_size as usize) };
    let entry_pointers = if entry_count == 0 {
        &[][..]
    } else {
        unsafe { slice::from_raw_parts(entries, entry_count as usize) }
    };
    let rust_entries = entry_pointers
        .iter()
        .map(|entry| {
            unsafe { CStr::from_ptr(*entry) }
                .to_string_lossy()
                .into_owned()
        })
        .collect();
    let result = lift(&model_yaml, binary, rust_entries, output as usize);
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

    // PipelineC owns this LLVMModuleRef. Module::new normally takes ownership,
    // so ManuallyDrop prevents Inkwell from disposing revng's module.
    let module = ManuallyDrop::new(unsafe { Module::new(output as _) });
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
            i64_type.const_int(1, false).into(),
            i32_type.const_int(u64::from(index == 0), false).into(),
            i32_type.const_zero().into(),
            null.into(),
        ];
        builder
            .build_call(newpc, &arguments, "")
            .map_err(|error| error.to_string())?;

        match instruction.kind {
            0 => {
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

fn lift(model_yaml: &str, binary: &[u8], entries: Vec<String>, output: usize) -> String {
    if !model_yaml.contains("x86_64") {
        return "the example Inkwell backend only supports x86_64".into();
    }
    let Some(entry_text) = entries
        .first()
        .map(String::as_str)
        .or_else(|| model_entry(model_yaml))
    else {
        return "the Inkwell backend needs an entry point".into();
    };
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

unsafe fn run_initialized(backend_name: &str) -> Result<u64, String> {
    let mut context = CallbackContext {
        bytes: [0x90, 0xc3],
        error: CString::new("").unwrap(),
    };
    let callbacks = revng_sys::rp_address_space_callbacks {
        opaque: ptr::from_mut(&mut context).cast(),
        architecture: Some(architecture),
        entry_point: Some(entry_point),
        mapping_count: Some(mapping_count),
        mapping_at: Some(mapping_at),
        extra_code_address_count: Some(extra_code_address_count),
        extra_code_address_at: None,
    };
    let error = unsafe { revng_sys::rp_error_create() };
    if error.is_null() {
        return Err("rp_error_create failed".into());
    }
    let manager = unsafe {
        revng_sys::rp_manager_create_from_address_space(
            &callbacks,
            0,
            ptr::null(),
            b"\0".as_ptr().cast(),
            error,
        )
    };
    if manager.is_null() {
        let result = unsafe { pipeline_error(error, "failed to create revng manager") };
        unsafe { revng_sys::rp_error_destroy(error) };
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

    let step =
        unsafe { revng_sys::rp_manager_get_step_from_name(manager, b"lift\0".as_ptr().cast()) };
    let identifier = unsafe {
        revng_sys::rp_manager_get_container_identifier_from_name(
            manager,
            b"root.bc.zstd\0".as_ptr().cast(),
        )
    };
    let kind =
        unsafe { revng_sys::rp_manager_get_kind_from_name(manager, b"root\0".as_ptr().cast()) };
    if step.is_null() || identifier.is_null() || kind.is_null() {
        unsafe {
            revng_sys::rp_manager_destroy(manager);
            revng_sys::rp_error_destroy(error);
        }
        return Err("the example pipeline has no lift/root output".into());
    }

    let container = unsafe { revng_sys::rp_step_get_container(step, identifier) };
    // PipelineC requires a non-null array even when the component count is 0.
    let empty_path = [ptr::null()];
    let target = unsafe { revng_sys::rp_target_create(kind, 0, empty_path.as_ptr()) };
    if container.is_null() || target.is_null() {
        if !target.is_null() {
            unsafe { revng_sys::rp_target_destroy(target) };
        }
        unsafe {
            revng_sys::rp_manager_destroy(manager);
            revng_sys::rp_error_destroy(error);
        }
        return Err("failed to create the lift target".into());
    }
    let targets = [target.cast_const()];
    let module = unsafe {
        revng_sys::rp_manager_produce_targets(
            manager,
            step,
            container,
            targets.len() as u64,
            targets.as_ptr(),
            error,
        )
    };
    unsafe { revng_sys::rp_target_destroy(target) };
    let result = if module.is_null() {
        Err(unsafe { pipeline_error(error, "lift failed") })
    } else {
        let size = unsafe { revng_sys::rp_buffer_size(module) };
        unsafe { revng_sys::rp_buffer_destroy(module) };
        if size == 0 {
            Err("revng produced an empty module".into())
        } else {
            Ok(size)
        }
    };
    unsafe {
        revng_sys::rp_manager_destroy(manager);
        revng_sys::rp_error_destroy(error);
    }
    result
}

fn run(backend_name: &str) -> Result<u64, String> {
    let pipeline_option = CString::new(format!(
        "--pipeline-path={}",
        env!("REVNG_EXAMPLE_PIPELINE")
    ))
    .unwrap();
    let program = CString::new("revng-rust-inkwell-lifter-example").unwrap();
    let arguments = [program.as_ptr(), pipeline_option.as_ptr()];
    if !unsafe { revng_sys::rp_initialize(2, arguments.as_ptr(), 0, ptr::null_mut()) } {
        return Err("rp_initialize failed".into());
    }
    let result = unsafe { run_initialized(backend_name) };
    if !unsafe { revng_sys::rp_shutdown() } && result.is_ok() {
        return Err("rp_shutdown failed".into());
    }
    result
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
mod tests {
    use super::{decode_x86_64, model_entry, parse_address};

    #[test]
    fn parses_revng_meta_address() {
        assert_eq!(parse_address("0x400000:Code_x86_64").unwrap(), 0x400000);
    }

    #[test]
    fn finds_entry_in_model() {
        assert_eq!(
            model_entry("EntryPoint: 0x400000:Code_x86_64\n"),
            Some("0x400000:Code_x86_64")
        );
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
    fn rejects_unsupported_or_unterminated_input() {
        assert!(decode_x86_64(&[0xcc], 0x400000)
            .unwrap_err()
            .contains("unsupported opcode"));
        assert!(decode_x86_64(&[0x90], 0x400000)
            .unwrap_err()
            .contains("no RET"));
    }
}
