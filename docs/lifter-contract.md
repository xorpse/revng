# Lifter backend contract

An implementation of `revng::lift::ILifter` produces the initial LLVM module
consumed by rev.ng's analysis and decompilation pipeline. A conforming backend
must provide the following observable IR shape.

1. A `void root(...)` function tagged with `FunctionTags::Root`. Its first
   argument represents the initial stack pointer. The function contains the
   whole-program control-flow graph known at lift time.
2. Calls to `newpc` delimit every input instruction and encode its address,
   size, and jump-target status. Later invalidation and isolation passes recover
   instruction boundaries and jump targets from these markers.
3. CPU state variables are LLVM globals with stable architecture-appropriate
   names (for example `_rax`, `_rsp`, and `_rip`, with `_state_0x<offset>` as a
   fallback). Their types and meanings must remain consistent throughout the
   module. `ProgramCounterHandler` also expects the scalar globals `pc_epoch`
   (`i32`), `pc_address_space` (`i16`), and `pc_type` (`i16`).
4. The root CFG includes dispatcher, `anyPC`, and `unexpectedPC` blocks and uses
   the architecture's `ProgramCounterHandler` conventions. The terminators of
   the dispatcher, dispatcher-failure, `anyPC`, and `unexpectedPC` blocks carry
   their corresponding `revng.block.type` metadata.

A backend can emit self-contained, helper-free IR. If it emits QEMU-style
helpers, every helper declaration and call must additionally carry authoritative
`revng.csvaccess.offsets.load` and `revng.csvaccess.offsets.store` metadata, and
the module must maintain `cpu_loop_exiting` around exiting helper calls.

Backends return an `llvm::Error` for unsupported architectures, invalid input,
or contract-generation failures. Returning success asserts that the output is
ready for the post-lift verifier and downstream pipeline.

## Architecture boundary

The backend registry is open-ended, but the model's guest-architecture enum is
currently closed: `x86`, `x86_64`, `arm`, `aarch64`, `mips`, `mipsel`, and
`systemz`. A backend can support any subset of those guests and must reject the
others cleanly. Adding a new guest architecture requires extending the model
schema and its register, pointer-size, ABI, and program-counter conventions;
registering a new backend name alone is not sufficient.

The host architecture is independent. For example, a native macOS AArch64
process can use `reference-x86_64` or a Rust x86-64 callback backend. libtcg
supports the model's existing guest set on Linux; it is not built on macOS.

## Helper-free backends

The minimum helper-free contract is the four unconditional items above. In
particular, a helper-free module does not need QEMU architecture metadata,
helper sections, CSV-access metadata, or `cpu_loop_exiting`.

The following consumers have been audited:

- post-lift verification accepts the self-contained instruction operations,
  CSV loads/stores, `newpc`, branches, switches, and unreachable dispatcher
  exits emitted by a helper-free backend;
- jump-target collection depends only on the `root` tag and `newpc` calls;
- helper linking first checks whether any declaration in the inline-helper
  section lacks a body, and is a no-op before consulting QEMU metadata or
  helper bitcode when none exists;
- helper inlining scans calls carrying the inline-helper section and is
  naturally a no-op when there are none;
- function isolation treats helper metadata conditionally. Direct CSV uses are
  discovered independently, while CSV-access metadata is queried only for
  calls tagged as helpers.

`revngLiftReference` is the executable contract example. Its intentionally
small x86-64 backend recognizes NOP and RET, emits `_rsp`/`_rip`, `newpc`
markers, and dispatcher blocks, and contains no helper calls. The PipelineC
regression runs it through the real `lift` pipe in a
`REVNG_BACKEND_LIBTCG=OFF` build.

## Selecting a backend from C

An address-space manager can select any linked, registered backend with
`rp_manager_set_lifter_backend`. The selection belongs to that manager's model
and does not change the process-wide default. A host can instead install its
own callback implementation with `rp_set_lifter`.

[`examples/lifter-library.c`](../examples/lifter-library.c) is a complete C
consumer. Its test target links both `revngPipelineC` and
`revngLiftReference`, supplies an in-memory NOP/RET mapping, and runs the lift
pipe with either `reference-x86_64` or `libtcg`:

```sh
test_lifter_library_c reference-x86_64
test_lifter_library_c libtcg
```

[`examples/rust-lifter`](../examples/rust-lifter) shows the same embedding
boundary from Rust using `cxx`. Rust owns instruction decoding and the backend
callback; a small C++ adapter emits the LLVM module and its revng metadata.

[`examples/rust-inkwell-lifter`](../examples/rust-inkwell-lifter) is the
corresponding Inkwell variant. Rust creates the LLVM IR directly; its narrow
C++ shim only applies revng-specific tags, block metadata, and `BasicBlockID`
constants. Both Rust examples can select their custom callback, the reference
backend, or libtcg from the same executable.

Reusable Rust bindings and relocatable SDK discovery live under
[`rust/revng-sys`](../rust/revng-sys) and
[`rust/revng-sdk`](../rust/revng-sdk). The SDK manifest records the generated
headers, matching LLVM installation, runtime libraries, pipelines, and backend
DSOs without exposing the revng build-tree layout to consumers.
