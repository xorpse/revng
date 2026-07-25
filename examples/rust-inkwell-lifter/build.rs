use revng_build::Sdk;

fn main() {
    let sdk = Sdk::discover().unwrap_or_else(|error| panic!("{error}"));
    sdk.configure_linkage();

    let mut bridge = cxx_build::bridge("src/main.rs");
    sdk.configure_cxx(&mut bridge);
    bridge.compile("revng-rust-inkwell-cxx");

    let mut tags = cc::Build::new();
    tags.file("cxx/src/revng_tags.cc");
    sdk.configure_cxx(&mut tags);
    tags.compile("revng-rust-inkwell-tags");

    sdk.emit_pipeline_env("address-space", "REVNG_EXAMPLE_PIPELINE")
        .unwrap_or_else(|error| panic!("{error}"));
    sdk.emit_pipeline_env("full", "REVNG_FULL_PIPELINE")
        .unwrap_or_else(|error| panic!("{error}"));

    println!("cargo::rerun-if-changed=src/main.rs");
    println!("cargo::rerun-if-changed=cxx");
}
