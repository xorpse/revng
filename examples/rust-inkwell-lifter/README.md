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

Native macOS AArch64 support is experimental and uses revng's pinned LLVM/MLIR
fork. It supports the `inkwell` and `reference-x86_64` backends. libtcg remains
Linux-only and is never included in a macOS SDK manifest.

## Full pipeline to C

The package also contains `revng-rust-decompile-example`. It requires a full
decompiler build (a minimal `REVNG_SDK_BUILD=ON` tree does not contain the C
emission pipeline). It starts from the
same in-memory address space but requests the final single-file C artifact, so
revng runs lifting, isolation, ABI enforcement, restructuring, Clift lowering,
and C emission. Run it with either the Inkwell callback or the built-in
reference backend:

```sh
REVNG_SDK_MANIFEST=/path/to/revng-sdk.json \
  cargo run --bin revng-rust-decompile-example -- inkwell inkwell.c
REVNG_SDK_MANIFEST=/path/to/revng-sdk.json \
  cargo run --bin revng-rust-decompile-example -- reference-x86_64 reference.c
```

PipelineC returns the standard C+PTML artifact. The example decodes that markup
in Rust and writes ordinary C text to the requested output path.

The SDK manifest used by this executable must contain both pipeline entries:

```sh
python scripts/generate-revng-sdk-manifest.py \
  ... \
  --pipeline address-space=stage-decompiler/share/revng/pipelines/address-space.yml \
  --pipeline full=stage-decompiler/share/revng/pipelines/revng-pipelines.yml \
  --output stage-decompiler/revng-sdk.json
```
