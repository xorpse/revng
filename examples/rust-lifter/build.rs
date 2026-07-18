use std::env;
use std::path::PathBuf;

fn main() {
    let sdk = revng_sdk::Sdk::discover().unwrap_or_else(|error| panic!("{error}"));
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap();
    let target_arch = env::var("CARGO_CFG_TARGET_ARCH").unwrap();
    sdk.validate_target(&target_os, &target_arch)
        .unwrap_or_else(|error| panic!("{error}"));
    sdk.emit_cargo_link_directives();

    env::set_var("CXX", sdk.compiler());
    let manifest = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let mut bridge = cxx_build::bridge("src/main.rs");
    bridge
        .file("src/bridge.cc")
        .include(&manifest)
        .flag("-std=c++17")
        .flag("-stdlib=libc++")
        .flag("-fno-rtti");
    for include in sdk.include_dirs() {
        bridge.include(include);
    }
    bridge.compile("revng-rust-lifter-cxx");

    let generated = PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("cxxbridge");
    let mut adapter = cc::Build::new();
    adapter
        .cpp(true)
        .warnings(false)
        .compiler(sdk.compiler())
        .file("src/ir_adapter.cc")
        .include(&manifest)
        .include(generated.join("include"))
        .include(generated.join("crate"))
        .flag("-std=c++20")
        .flag("-stdlib=libc++")
        .flag("-fno-rtti");
    for include in sdk.include_dirs() {
        adapter.include(include);
    }
    adapter.compile("revng-rust-lifter-adapter");

    let pipeline = sdk
        .pipeline("address-space")
        .unwrap_or_else(|| panic!("SDK manifest has no 'address-space' pipeline"));
    println!(
        "cargo:rustc-env=REVNG_EXAMPLE_PIPELINE={}",
        pipeline.display()
    );
    sdk.emit_cargo_rerun_directives();
    println!("cargo:rerun-if-changed=src/main.rs");
    println!("cargo:rerun-if-changed=src/bridge.cc");
    println!("cargo:rerun-if-changed=src/ir_adapter.cc");
    println!("cargo:rerun-if-changed=include/bridge.h");
}
