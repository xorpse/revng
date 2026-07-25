use std::env;

const INKWELL_LLVM_MAJOR: u32 = 16;

fn main() {
    let llvm_major = env::var("DEP_REVNG_LLVM_MAJOR")
        .expect("DEP_REVNG_LLVM_MAJOR is set by revng-sys")
        .parse::<u32>()
        .expect("DEP_REVNG_LLVM_MAJOR is an integer");
    assert_eq!(
        llvm_major, INKWELL_LLVM_MAJOR,
        "the revng SDK provides LLVM {llvm_major}, but the inkwell feature pins LLVM {INKWELL_LLVM_MAJOR}"
    );
}
