use std::env;

fn main() {
    let sdk = revng_sdk::Sdk::discover().unwrap_or_else(|error| panic!("{error}"));
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap();
    let target_arch = env::var("CARGO_CFG_TARGET_ARCH").unwrap();
    sdk.validate_target(&target_os, &target_arch)
        .unwrap_or_else(|error| panic!("{error}"));
    sdk.emit_cargo_link_directives();
    sdk.emit_cargo_rerun_directives();
    println!("cargo:metadata=manifest={}", sdk.manifest_path().display());
    println!("cargo:metadata=llvm_major={}", sdk.manifest().llvm_major);
}
