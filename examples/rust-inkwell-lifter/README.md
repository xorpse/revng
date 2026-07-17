# revng Rust/Inkwell lifter example

This is a second Rust backend example. Unlike `../rust-lifter`, it constructs
the LLVM module directly with Inkwell. The small C++20 shim only supplies the
revng-specific function tags, block metadata, and `BasicBlockID` constant that
are not part of LLVM's C API.

The example consumes the reusable `../../rust/revng-sys` crate. Its build is
described by a relocatable `revng-sdk.json`, instead of repository-relative
build and dependency paths. Generate one as described in
[`../../rust/README.md`](../../rust/README.md), then run:

```sh
REVNG_SDK_MANIFEST=/path/to/revng-sdk.json cargo run -- inkwell
REVNG_SDK_MANIFEST=/path/to/revng-sdk.json cargo run -- reference-x86_64
REVNG_SDK_MANIFEST=/path/to/revng-sdk-with-libtcg.json cargo run -- libtcg
```

The example lifts a two-byte x86-64 `NOP; RET` input. It can also select the
reference and libtcg backends registered in the same process.

The wrapped `LLVMModuleRef` remains owned by PipelineC. It is placed in
`ManuallyDrop` so Inkwell cannot dispose it when the callback returns.

Native macOS AArch64 support is experimental and uses Homebrew LLVM 16. It
supports the `inkwell` and `reference-x86_64` backends. libtcg remains
Linux-only and is never included in a macOS SDK manifest.
