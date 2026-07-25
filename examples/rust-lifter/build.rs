use revng_build::Sdk;

fn main() {
    let sdk = Sdk::discover().unwrap_or_else(|error| panic!("{error}"));
    sdk.configure_linkage();

    let mut bridge = cxx_build::bridge("src/main.rs");
    bridge.file("cxx/src/bridge.cc");
    sdk.configure_cxx(&mut bridge);
    bridge.compile("revng-rust-lifter-cxx");

    let mut adapter = cc::Build::new();
    adapter.file("cxx/src/ir_adapter.cc");
    sdk.configure_cxx(&mut adapter);
    adapter.compile("revng-rust-lifter-adapter");

    sdk.emit_pipeline_env("address-space", "REVNG_EXAMPLE_PIPELINE")
        .unwrap_or_else(|error| panic!("{error}"));

    println!("cargo::rerun-if-changed=src/main.rs");
    println!("cargo::rerun-if-changed=cxx");
}
