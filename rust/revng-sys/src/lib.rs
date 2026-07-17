#![allow(non_camel_case_types)]

use std::ffi::{c_char, c_int, c_void};

pub type LLVMModuleRef = *mut c_void;

macro_rules! opaque {
    ($($name:ident),+ $(,)?) => {$(
        #[repr(C)]
        pub struct $name {
            _private: [u8; 0],
        }
    )+};
}

opaque!(
    rp_manager,
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
    pub contents: *const u8,
    pub contents_size: u64,
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
    pub extra_code_address_count: Option<unsafe extern "C" fn(*mut c_void) -> u64>,
    pub extra_code_address_at: Option<unsafe extern "C" fn(*mut c_void, u64) -> *const c_char>,
}

pub type rp_lift_callback = unsafe extern "C" fn(
    opaque: *mut c_void,
    model_yaml: *const c_char,
    binary: *const u8,
    binary_size: u64,
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
    pub fn rp_manager_set_lifter_backend(
        manager: *mut rp_manager,
        name: *const c_char,
        error: *mut rp_error,
    ) -> bool;
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
