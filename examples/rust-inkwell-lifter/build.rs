use std::env;

fn main() {
    let sdk = revng_sdk::Sdk::discover().unwrap_or_else(|error| panic!("{error}"));
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap();
    let target_arch = env::var("CARGO_CFG_TARGET_ARCH").unwrap();
    sdk.validate_target(&target_os, &target_arch)
        .unwrap_or_else(|error| panic!("{error}"));
    // Native library search paths propagate through revng-sys, but final-link
    // options such as executable RPATHs and constructor-retained backend DSOs
    // must be emitted by the final Cargo package.
    sdk.emit_cargo_link_directives();

    env::set_var("CXX", sdk.compiler());
    let mut bridge = cxx_build::bridge("src/main.rs");
    bridge.include(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    for include in sdk.include_dirs() {
        bridge.include(include);
    }
    bridge
        .flag("-std=c++17")
        .flag("-fno-rtti")
        .compile("revng-rust-inkwell-cxx");

    let mut tags = cc::Build::new();
    tags.cpp(true)
        .warnings(false)
        .compiler(sdk.compiler())
        .file("src/revng_tags.cc")
        .flag("-std=c++20")
        .flag("-stdlib=libc++")
        .flag("-fno-rtti");
    for include in sdk.include_dirs() {
        tags.include(include);
    }
    tags.compile("revng-rust-inkwell-tags");

    let pipeline = sdk
        .pipeline("address-space")
        .unwrap_or_else(|| panic!("SDK manifest has no 'address-space' pipeline"));
    println!(
        "cargo:rustc-env=REVNG_EXAMPLE_PIPELINE={}",
        pipeline.display()
    );
    sdk.emit_cargo_rerun_directives();
    println!("cargo:rerun-if-changed=src/main.rs");
    println!("cargo:rerun-if-changed=src/revng_tags.cc");
    println!("cargo:rerun-if-changed=include/bridge.h");
}
