#![allow(non_camel_case_types)]

use std::ffi::{c_char, c_int, c_void};

pub type LLVMModuleRef = *mut c_void;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct rp_mlir_module {
    pub ptr: *const c_void,
}

macro_rules !opaque {
    ($($name:ident),+ $(,)?) => {$(
#[repr(C)]
        pub struct $name {
  _private:
    [u8; 0],
        }
    )+};
}

opaque!(
    rp_manager,
    rp_binary_view,
    rp_kind,
    rp_step,
    rp_container,
    rp_container_identifier,
    rp_target,
    rp_error,
    rp_document_error,
    rp_simple_error,
    rp_buffer,
);

#[repr(C)]
pub struct rp_address_space_mapping {
    pub start: *const c_char,
    pub virtual_size: u64,
    pub backing_size: u64,
    pub readable: bool,
    pub writeable: bool,
    pub executable: bool,
    pub name: *const c_char,
}

#[repr(C)]
pub struct rp_address_space_callbacks {
    pub opaque: *mut c_void,
    pub architecture: Option<unsafe extern "C" fn(*mut c_void) -> *const c_char>,
    pub entry_point: Option<unsafe extern "C" fn(*mut c_void) -> *const c_char>,
    pub mapping_count: Option<unsafe extern "C" fn(*mut c_void) -> u64>,
    pub mapping_at:
        Option<unsafe extern "C" fn(*mut c_void, u64, *mut rp_address_space_mapping) -> bool>,
    pub read: Option<unsafe extern "C" fn(*mut c_void, u64, u64, *mut u8, u64) -> bool>,
    pub extra_code_address_count: Option<unsafe extern "C" fn(*mut c_void) -> u64>,
    pub extra_code_address_at: Option<unsafe extern "C" fn(*mut c_void, u64) -> *const c_char>,
    pub release: Option<unsafe extern "C" fn(*mut c_void)>,
}

#[repr(C)]
pub struct rp_file_address_space_mapping {
    pub start: *const c_char,
    pub virtual_size: u64,
    pub backing_size: u64,
    pub path: *const c_char,
    pub file_offset: u64,
    pub readable: bool,
    pub writeable: bool,
    pub executable: bool,
    pub name: *const c_char,
}

pub type rp_lift_callback = unsafe extern "C" fn(
    opaque: *mut c_void,
    model_yaml: *const c_char,
    binary: *const rp_binary_view,
    entries: *const *const c_char,
    entry_count: u64,
    output: LLVMModuleRef,
    error_message: *mut *const c_char,
) -> bool;

#[repr(C)]
pub struct rp_lifter_callbacks {
    pub opaque: *mut c_void,
    pub lift: Option<rp_lift_callback>,
}

pub type rp_llvm_module_transform_callback = unsafe extern "C" fn(
    opaque: *mut c_void,
    module: LLVMModuleRef,
    error_message: *mut *const c_char,
) -> bool;

#[repr(C)]
pub struct rp_llvm_module_callbacks {
    pub opaque: *mut c_void,
    pub transform: Option<rp_llvm_module_transform_callback>,
}

pub type rp_mlir_module_transform_callback = unsafe extern "C" fn(
    opaque: *mut c_void,
    module: rp_mlir_module,
    error_message: *mut *const c_char,
) -> bool;

#[repr(C)]
pub struct rp_mlir_module_callbacks {
    pub opaque: *mut c_void,
    pub transform: Option<rp_mlir_module_transform_callback>,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub enum rp_primitive_kind {
    RP_PRIMITIVE_KIND_VOID = 0,
    RP_PRIMITIVE_KIND_GENERIC,
    RP_PRIMITIVE_KIND_POINTER_OR_NUMBER,
    RP_PRIMITIVE_KIND_NUMBER,
    RP_PRIMITIVE_KIND_UNSIGNED,
    RP_PRIMITIVE_KIND_SIGNED,
    RP_PRIMITIVE_KIND_FLOAT,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct rp_primitive_type {
    pub kind: rp_primitive_kind,
    pub size: u64,
}

#[repr(C)]
pub struct rp_cabi_argument {
    pub name: *const c_char,
    pub type_: rp_primitive_type,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub enum rp_type_kind {
    RP_TYPE_KIND_PRIMITIVE = 0,
    RP_TYPE_KIND_POINTER,
    RP_TYPE_KIND_ARRAY,
    RP_TYPE_KIND_DEFINED,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct rp_type {
    pub kind: rp_type_kind,
    pub is_const: bool,
    pub primitive: rp_primitive_type,
    pub size: u64,
    pub definition_id: u64,
    pub element_type: *const rp_type,
}

#[repr(C)]
pub struct rp_typed_argument {
    pub name: *const c_char,
    pub comment: *const c_char,
    pub type_: rp_type,
}

#[repr(C)]
pub struct rp_named_typed_register {
    pub register_name: *const c_char,
    pub name: *const c_char,
    pub comment: *const c_char,
    pub type_: rp_type,
}

extern "C" {
    pub fn rp_initialize(
        argc: c_int,
        argv: *const *const c_char,
        signals_to_preserve_count: u32,
        signals_to_preserve: *mut c_int,
    ) -> bool;
    pub fn rp_shutdown() -> bool;
    pub fn rp_manager_create_from_address_space(
        callbacks: *const rp_address_space_callbacks,
        materialize_for_serialization: u64,
        pipeline_flags_count: u64,
        pipeline_flags: *const *const c_char,
        execution_directory: *const c_char,
        error: *mut rp_error,
    ) -> *mut rp_manager;
    pub fn rp_manager_create_from_file_address_space(
        architecture: *const c_char,
        entry_point: *const c_char,
        mappings_count: u64,
        mappings: *const rp_file_address_space_mapping,
        materialize_for_serialization: u64,
        pipeline_flags_count: u64,
        pipeline_flags: *const *const c_char,
        execution_directory: *const c_char,
        error: *mut rp_error,
    ) -> *mut rp_manager;
    pub fn rp_set_lifter(
        manager: *mut rp_manager,
        callbacks: *const rp_lifter_callbacks,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_binary_view_size(binary: *const rp_binary_view) -> u64;
    pub fn rp_binary_view_read_offset(
        binary: *const rp_binary_view,
        offset: u64,
        size: u64,
        destination: *mut u8,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_binary_view_read_address(
        binary: *const rp_binary_view,
        address: *const c_char,
        size: u64,
        destination: *mut u8,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_manager_materialize_address_space(
        manager: *mut rp_manager,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_manager_set_lifter_backend(
        manager: *mut rp_manager,
        name: *const c_char,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_manager_set_cabi_prototype(
        manager: *mut rp_manager,
        address: *const c_char,
        abi: *const c_char,
        function_name: *const c_char,
        arguments_count: u64,
        arguments: *const rp_cabi_argument,
        return_type: *const rp_primitive_type,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_manager_set_default_abi(
        manager: *mut rp_manager,
        abi: *const c_char,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_manager_set_target_abi(
        manager: *mut rp_manager,
        abi: *const c_char,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_manager_set_operating_system(
        manager: *mut rp_manager,
        operating_system: *const c_char,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_manager_set_platform_name(
        manager: *mut rp_manager,
        platform_name: *const c_char,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_manager_set_entry_point(
        manager: *mut rp_manager,
        address: *const c_char,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_manager_add_extra_code_address(
        manager: *mut rp_manager,
        address: *const c_char,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_manager_add_function(
        manager: *mut rp_manager,
        address: *const c_char,
        name: *const c_char,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_manager_set_function_prototype(
        manager: *mut rp_manager,
        address: *const c_char,
        type_definition_id: u64,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_manager_create_struct_type(
        manager: *mut rp_manager,
        name: *const c_char,
        comment: *const c_char,
        size: u64,
        can_contain_code: u64,
        error: *mut rp_error,
    ) -> u64;
    pub fn rp_manager_add_struct_field(
        manager: *mut rp_manager,
        type_definition_id: u64,
        offset: u64,
        name: *const c_char,
        comment: *const c_char,
        type_: *const rp_type,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_manager_create_union_type(
        manager: *mut rp_manager,
        name: *const c_char,
        comment: *const c_char,
        error: *mut rp_error,
    ) -> u64;
    pub fn rp_manager_add_union_field(
        manager: *mut rp_manager,
        type_definition_id: u64,
        name: *const c_char,
        comment: *const c_char,
        type_: *const rp_type,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_manager_create_enum_type(
        manager: *mut rp_manager,
        name: *const c_char,
        comment: *const c_char,
        underlying_type: *const rp_primitive_type,
        error: *mut rp_error,
    ) -> u64;
    pub fn rp_manager_add_enum_entry(
        manager: *mut rp_manager,
        type_definition_id: u64,
        value: u64,
        name: *const c_char,
        comment: *const c_char,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_manager_create_typedef(
        manager: *mut rp_manager,
        name: *const c_char,
        comment: *const c_char,
        underlying_type: *const rp_type,
        error: *mut rp_error,
    ) -> u64;
    pub fn rp_manager_create_cabi_type(
        manager: *mut rp_manager,
        name: *const c_char,
        comment: *const c_char,
        abi: *const c_char,
        arguments_count: u64,
        arguments: *const rp_typed_argument,
        return_type: *const rp_type,
        return_value_comment: *const c_char,
        error: *mut rp_error,
    ) -> u64;
    pub fn rp_manager_create_raw_function_type(
        manager: *mut rp_manager,
        name: *const c_char,
        comment: *const c_char,
        architecture: *const c_char,
        arguments_count: u64,
        arguments: *const rp_named_typed_register,
        return_values_count: u64,
        return_values: *const rp_named_typed_register,
        preserved_registers_count: u64,
        preserved_registers: *const *const c_char,
        final_stack_offset: u64,
        stack_arguments_type: *const rp_type,
        return_value_comment: *const c_char,
        error: *mut rp_error,
    ) -> u64;
    pub fn rp_manager_add_function_exported_name(
        manager: *mut rp_manager,
        address: *const c_char,
        name: *const c_char,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_manager_add_imported_library(
        manager: *mut rp_manager,
        name: *const c_char,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_manager_add_imported_function(
        manager: *mut rp_manager,
        name: *const c_char,
        type_definition_id: u64,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_manager_add_data_symbol(
        manager: *mut rp_manager,
        address: *const c_char,
        name: *const c_char,
        type_: *const rp_type,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_lifter_backend_count() -> u64;
    pub fn rp_lifter_backend_name(index: u64) -> *mut c_char;
    pub fn rp_lifter_backend_supports_architecture(
        backend: *const c_char,
        architecture: *const c_char,
    ) -> bool;
    pub fn rp_architecture_count() -> u64;
    pub fn rp_architecture_name(index: u64) -> *mut c_char;
    pub fn rp_abi_count(architecture: *const c_char) -> u64;
    pub fn rp_abi_name(architecture: *const c_char, index: u64) -> *mut c_char;
    pub fn rp_string_destroy(string: *mut c_char);
    pub fn rp_manager_decompile_to_ptml(
        manager: *mut rp_manager,
        error: *mut rp_error,
    ) -> *mut rp_buffer;
    pub fn rp_manager_decompile_to_c(
        manager: *mut rp_manager,
        error: *mut rp_error,
    ) -> *mut rp_buffer;
    pub fn rp_manager_decompile_to_c_bundle(
        manager: *mut rp_manager,
        error: *mut rp_error,
    ) -> *mut rp_buffer;
    pub fn rp_manager_decompile_function_to_ptml(
        manager: *mut rp_manager,
        address: *const c_char,
        error: *mut rp_error,
    ) -> *mut rp_buffer;
    pub fn rp_manager_decompile_function_to_c(
        manager: *mut rp_manager,
        address: *const c_char,
        error: *mut rp_error,
    ) -> *mut rp_buffer;
    pub fn rp_manager_produce_artifact(
        manager: *mut rp_manager,
        step_name: *const c_char,
        container_name: *const c_char,
        kind_name: *const c_char,
        path_components_count: u64,
        path_components: *const *const c_char,
        error: *mut rp_error,
    ) -> *mut rp_buffer;
    pub fn rp_manager_transform_llvm_module(
        manager: *mut rp_manager,
        step_name: *const c_char,
        container_name: *const c_char,
        callbacks: *const rp_llvm_module_callbacks,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_manager_transform_mlir_module(
        manager: *mut rp_manager,
        step_name: *const c_char,
        container_name: *const c_char,
        callbacks: *const rp_mlir_module_callbacks,
        error: *mut rp_error,
    ) -> bool;
    pub fn rp_manager_save(manager: *mut rp_manager) -> bool;
    pub fn rp_manager_destroy(manager: *mut rp_manager);
    pub fn rp_manager_get_container_identifier_from_name(
        manager: *const rp_manager,
        name: *const c_char,
    ) -> *const rp_container_identifier;
    pub fn rp_manager_get_step_from_name(
        manager: *mut rp_manager,
        name: *const c_char,
    ) -> *mut rp_step;
    pub fn rp_manager_get_kind_from_name(
        manager: *const rp_manager,
        name: *const c_char,
    ) -> *const rp_kind;
    pub fn rp_manager_produce_targets(
        manager: *mut rp_manager,
        step: *const rp_step,
        container: *const rp_container,
        targets_count: u64,
        targets: *const *const rp_target,
        error: *mut rp_error,
    ) -> *mut rp_buffer;
    pub fn rp_step_get_container(
        step: *mut rp_step,
        identifier: *const rp_container_identifier,
    ) -> *mut rp_container;
    pub fn rp_target_create(
        kind: *const rp_kind,
        path_components_count: u64,
        path_components: *const *const c_char,
    ) -> *mut rp_target;
    pub fn rp_target_destroy(target: *mut rp_target);
    pub fn rp_error_create() -> *mut rp_error;
    pub fn rp_error_destroy(error: *mut rp_error);
    pub fn rp_error_get_document_error(error: *mut rp_error) -> *mut rp_document_error;
    pub fn rp_error_get_simple_error(error: *mut rp_error) -> *mut rp_simple_error;
    pub fn rp_document_error_reasons_count(error: *const rp_document_error) -> u64;
    pub fn rp_document_error_get_error_message(
        error: *const rp_document_error,
        index: u64,
    ) -> *const c_char;
    pub fn rp_simple_error_get_message(error: *const rp_simple_error) -> *const c_char;
    pub fn rp_buffer_size(buffer: *const rp_buffer) -> u64;
    pub fn rp_buffer_data(buffer: *const rp_buffer) -> *const c_char;
    pub fn rp_buffer_destroy(buffer: *mut rp_buffer);
}

#[cfg(test)]
mod tests {
    use super::{rp_address_space_callbacks, rp_address_space_mapping, rp_lifter_callbacks};

    #[test]
    fn callback_structs_have_c_compatible_alignment() {
        assert_eq!(
            std::mem::align_of::<rp_address_space_mapping>(),
            std::mem::align_of::<usize>()
        );
        assert_eq!(
            std::mem::align_of::<rp_address_space_callbacks>(),
            std::mem::align_of::<usize>()
        );
        assert_eq!(
            std::mem::align_of::<rp_lifter_callbacks>(),
            std::mem::align_of::<usize>()
        );
    }
}
