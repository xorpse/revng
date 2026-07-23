//! Safe ownership adapters between revng's borrowed LLVM handles and Inkwell.

use std::ffi::{c_char, c_void, CStr, CString};
use std::mem::ManuallyDrop;
use std::ops::Deref;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;

use inkwell::llvm_sys::error::{LLVMDisposeErrorMessage, LLVMGetErrorMessage};
use inkwell::llvm_sys::transforms::pass_builder::{
    LLVMCreatePassBuilderOptions, LLVMDisposePassBuilderOptions, LLVMRunPasses,
};
use inkwell::module::Module;

/// An Inkwell view of an LLVM module owned by revng.
///
/// The view never disposes the underlying LLVM module. Its lifetime is scoped
/// to the revng callback which supplied the handle.
pub struct BorrowedModule<'callback> {
    module: ManuallyDrop<Module<'callback>>,
}

impl<'callback> BorrowedModule<'callback> {
    /// Return the ordinary Inkwell module reference used by APIs such as pass
    /// managers whose generic parameter must be exactly `Module`.
    pub fn as_module(&self) -> &Module<'callback> {
        &self.module
    }

    /// Run a target-independent LLVM new-pass-manager pipeline.
    ///
    /// `pipeline` uses the same syntax as `opt -passes`, for example
    /// `"instcombine,reassociate"`.
    pub fn run_passes(&self, pipeline: &str) -> Result<(), String> {
        let pipeline = CString::new(pipeline)
            .map_err(|_| "LLVM pass pipeline contains a NUL byte".to_owned())?;
        unsafe {
            let options = LLVMCreatePassBuilderOptions();
            let error = LLVMRunPasses(
                self.module.as_mut_ptr(),
                pipeline.as_ptr(),
                ptr::null_mut(),
                options,
            );
            LLVMDisposePassBuilderOptions(options);
            if error.is_null() {
                return Ok(());
            }

            let message = LLVMGetErrorMessage(error);
            let result = if message.is_null() {
                "LLVM pass pipeline failed without an error message".to_owned()
            } else {
                CStr::from_ptr(message).to_string_lossy().into_owned()
            };
            if !message.is_null() {
                LLVMDisposeErrorMessage(message);
            }
            Err(result)
        }
    }

    /// Wrap a module handle borrowed for `'callback`.
    ///
    /// # Safety
    ///
    /// `module` must be non-null and remain valid for `'callback`. The caller
    /// must guarantee exclusive access for mutations performed through the
    /// returned view.
    pub unsafe fn from_raw(module: revng_sys::LLVMModuleRef) -> Self {
        assert!(!module.is_null(), "revng supplied a null LLVM module");
        Self {
            module: ManuallyDrop::new(unsafe { Module::new(module.cast()) }),
        }
    }
}

impl<'callback> Deref for BorrowedModule<'callback> {
    type Target = Module<'callback>;

    fn deref(&self) -> &Self::Target {
        self.as_module()
    }
}

struct TransformState<F> {
    transform: F,
    error: Option<CString>,
}

impl<F> TransformState<F> {
    fn fail(&mut self, message: impl Into<String>, error_message: *mut *const c_char) -> bool {
        let message = message.into().replace('\0', "\\0");
        self.error = Some(CString::new(message).expect("NUL bytes were replaced"));
        if !error_message.is_null() {
            unsafe { *error_message = self.error.as_ref().unwrap().as_ptr() };
        }
        false
    }
}

unsafe extern "C" fn transform_trampoline<F>(
    opaque: *mut c_void,
    module: revng_sys::LLVMModuleRef,
    error_message: *mut *const c_char,
) -> bool
where
    F: for<'callback> FnMut(&BorrowedModule<'callback>) -> Result<(), String>,
{
    let state = unsafe { &mut *opaque.cast::<TransformState<F>>() };
    if module.is_null() {
        return state.fail("revng supplied a null LLVM module", error_message);
    }

    let result = catch_unwind(AssertUnwindSafe(|| {
        // PipelineC keeps this transactional clone alive for the duration of
        // the callback and verifies it before committing it.
        let module = unsafe { BorrowedModule::from_raw(module) };
        (state.transform)(&module)
    }));
    match result {
        Ok(Ok(())) => true,
        Ok(Err(message)) => state.fail(message, error_message),
        Err(_) => state.fail("Rust LLVM transform panicked", error_message),
    }
}

/// Transform a revng LLVM container through a scoped Inkwell module view.
///
/// revng commits the transformed clone only if the closure succeeds and LLVM
/// verification passes. The closure cannot take ownership of the supplied
/// module through this API.
///
/// # Safety
///
/// `manager` and `error` must be valid pointers accepted by PipelineC. They
/// must not be used concurrently for the duration of this call.
pub unsafe fn transform_llvm_module<F>(
    manager: *mut revng_sys::rp_manager,
    step_name: &CStr,
    container_name: &CStr,
    error: *mut revng_sys::rp_error,
    transform: F,
) -> bool
where
    F: for<'callback> FnMut(&BorrowedModule<'callback>) -> Result<(), String>,
{
    let mut state = TransformState {
        transform,
        error: None,
    };
    let callbacks = revng_sys::rp_llvm_module_callbacks {
        opaque: ptr::from_mut(&mut state).cast(),
        transform: Some(transform_trampoline::<F>),
    };
    unsafe {
        revng_sys::rp_manager_transform_llvm_module(
            manager,
            step_name.as_ptr(),
            container_name.as_ptr(),
            &callbacks,
            error,
        )
    }
}
