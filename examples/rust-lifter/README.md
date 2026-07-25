# Rust revng lifter backend

This example embeds revng through its C API and installs a lifter callback
implemented in Rust. `cxx` provides the Rust/C++ calls; the public revng ABI
remains C.

The Rust half decodes the in-memory x86-64 `NOP; RET` sample. A small C++ IR
adapter turns the decoded instructions into the `root` LLVM module required by
revng. Keeping this adapter in C++ avoids exposing LLVM C++ ownership and
revng's metadata helpers to Rust.

Build revng and export the SDK prefixes as described in
[`../../rust/README.md`](../../rust/README.md), then run:

```sh
cargo run -- rust
cargo run -- reference-x86_64
```

Use `libtcg` as the argument with a build configured with
`REVNG_BACKEND_LIBTCG=ON`. Native macOS AArch64 builds omit libtcg but support
both `rust` and `reference-x86_64`.
