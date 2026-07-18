# Embedding the decompiler

PipelineC is the stable C boundary for embedding revng in C, C++, Rust, or any
language with a C FFI. An application supplies an address space and model
metadata, selects a linked lifter backend or installs a callback backend, then
requests C directly from the full pipeline.

`examples/decompile-library.c` is the smallest complete C consumer.
`examples/rust-inkwell-lifter` implements the same path in Rust and includes a
backend that constructs revng-compatible LLVM IR with Inkwell.

## Supplying bytes

There are two input forms:

- `rp_manager_create_from_address_space` records mapping metadata without
  reading bytes. Its `read` callback receives a mapping index, a mapping-local
  offset, and an exact byte count. Requested ranges are cached and unbacked
  virtual tails read as zeroes. Pass a nonzero materialization flag when an
  eagerly owned, immediately serializable input is preferable.
- `rp_manager_create_from_file_address_space` is the lazy convenience form for
  file paths, offsets, and lengths. revng opens files for individual reads; the
  files must not be replaced or truncated while the manager is alive.

In lazy mode, callback state remains owned by the host. If a `release` callback
is supplied, revng invokes it exactly once when the manager is destroyed or
when the input is materialized. Mapping metadata strings only need to remain
valid for the duration of manager creation.

The `backing_size` of a mapping may be smaller than `virtual_size`, as with a
BSS-like region. revng calls the provider only within the backed prefix and
synthesizes zero bytes for the remainder.

## Serialization is opt-in

A truly lazy manager cannot be saved because its input container does not yet
own the complete image. `rp_manager_save` therefore returns false until the
address space has been materialized.

Pass a nonzero `materialize_for_serialization` value when creating the manager,
or call `rp_manager_materialize_address_space` later. Materialization reads the
entire flattened virtual address space, including zero-filled virtual tails,
installs it as the ordinary input container, releases the provider, and makes
normal pipeline serialization possible. This is deliberately an explicit
memory-cost decision.

## Backend callbacks

`rp_set_lifter` receives an opaque `rp_binary_view` and can call
`rp_binary_view_read_offset` or `rp_binary_view_read_address` for exact ranges.
The view is valid only during the lifter callback.

Built-in and plugin backends are selected with
`rp_manager_set_lifter_backend`. Applications can enumerate backend names with
`rp_lifter_backend_count`/`rp_lifter_backend_name` and query a backend's guest
support with `rp_lifter_backend_supports_architecture`. Architectures and their
valid ABI names are similarly exposed by `rp_architecture_*` and `rp_abi_*`.

The registry is open to additional backends, but the model's guest architecture
set is currently `x86`, `x86_64`, `arm`, `aarch64`, `mips`, `mipsel`, and
`systemz`. Extending that set requires model and ABI work, not just a backend
registration. Host and guest architectures are independent: an Apple Silicon
process can decompile x86-64 input through the reference or a callback backend.

## Describing the binary

The address-space constructor records architecture, mappings, an optional entry
point, and optional extra code addresses. The model APIs can then add or update:

- entry points, functions, exported names, imports, and data symbols;
- default and target ABI, operating system, and platform name;
- primitive, pointer, array, struct, union, enum, and typedef types;
- definition, field, enum-entry, argument, and return-value comments;
- explicit struct sizes and field offsets, plus `CanContainCode`;
- reusable C ABI and raw register-level function types and per-function
  prototypes. Raw types expose argument, return, and preserved registers,
  stack-argument structs, final stack offsets, and architecture.

`rp_manager_set_cabi_prototype` is the compact path for ordinary primitive
signatures. The `rp_type` APIs cover nested and reusable types. Type-creation
functions return `UINT64_MAX` on failure and otherwise return an ID usable by
later calls. The current revng C ABI model has no variadic-function field, so
variadic prototypes are not exposed by this API.

Explicit offsets and total sizes describe packed or otherwise non-natural
struct layouts. revng's model does not contain explicit alignment or bitfield
metadata; debug-info importers likewise lower bitfields to their underlying
types. Those two properties therefore cannot be preserved by any model-backed
API without first extending revng's core type system and C emitter.

The raw Rust declarations in `rust/revng-sys` mirror every type API and input
structure exactly, including `rp_typed_argument` and
`rp_named_typed_register`.

## Requesting output

`rp_manager_decompile_to_c` runs the configured full pipeline and returns the
single-file plain-C artifact. `rp_manager_decompile_function_to_c` requests one
known function. Matching `_to_ptml` variants retain revng's token markup for
clients that use semantic presentation data. Returned buffers are owned by the
caller and must be released with `rp_buffer_destroy`.

The full APIs require a full decompiler build and the `revng-pipelines.yml`
pipeline. A minimal SDK-only build contains the embedding and lifting boundary
but not all C-emission analyses. A distributable SDK should ship the PipelineC
library, the selected backend DSOs, the matching revng LLVM shared libraries,
pipeline YAML files, and their runtime search paths. macOS builds omit libtcg;
the reference and callback backends provide the same decompiler pipeline there.
