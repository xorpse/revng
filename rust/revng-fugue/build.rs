use std::env;

fn main() {
    let sdk = revng_sdk::Sdk::discover().unwrap_or_else(|error| panic!("{error}"));
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap();
    let target_arch = env::var("CARGO_CFG_TARGET_ARCH").unwrap();
    sdk.validate_target(&target_os, &target_arch)
        .unwrap_or_else(|error| panic!("{error}"));
    sdk.emit_cargo_link_directives();
    for library in ["revngLift", "revngStorage", "revngPipeline", "revngPipes"] {
        println!("cargo:rustc-link-lib=dylib={library}");
    }

    let mut bridge = cxx_build::bridge("src/tags.rs");
    bridge
        .compiler(sdk.compiler())
        .file("src/tags.cc")
        .file("src/lifter.cc")
        .warnings(false)
        .include(env::var_os("CARGO_MANIFEST_DIR").unwrap())
        .flag("-std=c++20")
        .flag("-stdlib=libc++")
        .flag("-fno-rtti");
    for include in sdk.include_dirs() {
        bridge.include(include);
    }
    for prefix in ["libarchive", "boost", "zstd"] {
        if let Ok(output) = std::process::Command::new("brew")
            .args(["--prefix", prefix])
            .output()
            && output.status.success()
        {
            let path = String::from_utf8_lossy(&output.stdout).trim().to_owned();
            bridge.include(format!("{path}/include"));
        }
    }
    bridge.compile("revng-fugue-cxx");

    let address_space = sdk
        .pipeline("address-space")
        .unwrap_or_else(|| panic!("SDK manifest has no 'address-space' pipeline"));
    println!(
        "cargo:rustc-env=REVNG_ADDRESS_SPACE_PIPELINE={}",
        address_space.display()
    );
    let full = sdk
        .pipeline("full")
        .unwrap_or_else(|| panic!("SDK manifest has no 'full' pipeline"));
    println!("cargo:rustc-env=REVNG_FULL_PIPELINE={}", full.display());

    sdk.emit_cargo_rerun_directives();
    println!("cargo:rerun-if-changed=src/tags.rs");
    println!("cargo:rerun-if-changed=src/tags.cc");
    println!("cargo:rerun-if-changed=include/tags.h");
    println!("cargo:rerun-if-changed=src/lifter.cc");
    println!("cargo:rerun-if-changed=include/lifter.h");
}
