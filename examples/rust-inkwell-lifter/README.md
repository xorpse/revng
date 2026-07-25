# revng Rust/Inkwell lifter example

This is a second Rust backend example. Unlike `../rust-lifter`, it constructs
the LLVM module directly with Inkwell. The small C++20 shim only supplies the
revng-specific function tags, block metadata, and `BasicBlockID` constant that
are not part of LLVM's C API.

The example consumes the reusable `../../rust/revng-sys` crate and locates the
staged SDK through the `REVNG_SDK` and `REVNG_LLVM` prefixes. Export them as
described in [`../../rust/README.md`](../../rust/README.md), then run:

```sh
cargo run -- inkwell
cargo run -- reference-x86_64
cargo run -- libtcg   # Linux SDKs only
```

The example lifts a five-byte x86-64 implementation of `int32_t add(int32_t a,
int32_t b)`. It assigns the entry point a System V x86-64 C ABI prototype and
can also select the reference and libtcg backends registered in the same
process.

The reusable `revng-inkwell` adapter presents PipelineC's `LLVMModuleRef` as a
scoped `BorrowedModule`. It hides Inkwell's owning raw-handle constructor, so
backend code cannot accidentally dispose revng's module when the callback
returns.

Native macOS AArch64 support is experimental and uses revng's pinned LLVM/MLIR
fork. It supports the `inkwell` and `reference-x86_64` backends. libtcg remains
Linux-only and is never staged in a macOS SDK.

## Full pipeline to C

The package also contains `revng-rust-decompile-example`. It requires a full
decompiler build (a minimal `REVNG_SDK_BUILD=ON` tree does not contain the C
emission pipeline). It starts from the
same in-memory address space but requests the final single-file C artifact, so
revng runs lifting, isolation, ABI enforcement, restructuring, Clift lowering,
and C emission. Run it with either the Inkwell callback or the built-in
reference backend:

```sh
cargo run --bin revng-rust-decompile-example -- inkwell inkwell.c
cargo run --bin revng-rust-decompile-example -- reference-x86_64 reference.c
```

The manager is backed by range-read callbacks, so creating it does not copy the
input image. The Inkwell callback uses `rp_binary_view_read_address` to request
only the instruction bytes it decodes. PipelineC's direct decompilation API
returns ordinary C text; callers that need token markup can request the PTML
variant instead.

The staged SDK used by this executable must ship both pipeline YAMLs under
`share/revng/pipelines`: `address-space.yml` and `revng-pipelines.yml`.
