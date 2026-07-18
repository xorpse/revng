# Rust SDK integration

`revng-sdk` discovers and validates a relocatable `revng-sdk.json` descriptor.
`revng-sys` provides the raw PipelineC callback ABI and emits the link settings
described by that manifest.

Generate a descriptor after building revng:

```sh
python sources/revng/scripts/generate-revng-sdk-manifest.py \
  --source-dir sources/revng \
  --build-dir sources/revng/build/revng/complete-off \
  --llvm-dir root/lib64/llvm/llvm \
  --cxx root/bin/clang++ \
  --runtime-lib-dir root/lib64 \
  --runtime-lib-dir root/lib64/llvm/clang-release/lib/x86_64-unknown-linux-gnu \
  --pipeline address-space=sources/revng/tests/unit/PipelineCAddressSpace.yml \
  --pipeline full=sources/revng/share/revng/pipelines/revng-pipelines.yml \
  --output revng-sdk.json
```

For a complete standalone Linux x86-64 source build, including the pinned
LLVM/MLIR, nanobind, and QEMU/libtcg dependencies, follow
[`../docs/linux-sdk.md`](../docs/linux-sdk.md).

Consumers set either `REVNG_SDK_MANIFEST=/path/to/revng-sdk.json` or
`REVNG_SDK=/path/to/sdk-directory`. All paths stored in the descriptor are
relative to the descriptor, so the described directory tree can be moved as a
unit.

Applications that need bundled-library RPATHs or constructor-registered
backends should also add `revng-sdk` as a build dependency and call
`Sdk::emit_cargo_link_directives()` from their final executable's `build.rs`.
Cargo does not propagate executable linker arguments from a transitive sys
crate. `revng-sys` still supplies the raw declarations and base native-library
dependency for library crates.

The supported SDK targets are Linux x86-64 and experimental native macOS
AArch64. Full macOS builds use revng's pinned LLVM 16 fork, libc++, and
relative `@rpath` entries; they support callback and reference backends but
exclude libtcg.
Other target pairs require the explicit `--allow-unsupported-target` flag.

For a native macOS AArch64 build, configure with
`-DREVNG_BACKEND_LIBTCG=OFF`, following the complete instructions in
[`../docs/macos-sdk.md`](../docs/macos-sdk.md). Generate a descriptor directly
from the installed SDK with:

```sh
python scripts/generate-revng-sdk-manifest.py \
  --sdk-root stage-decompiler \
  --llvm-dir revng-llvm-install \
  --cxx /usr/bin/clang++ \
  --pipeline address-space=stage-decompiler/share/revng/pipelines/address-space.yml \
  --pipeline full=stage-decompiler/share/revng/pipelines/revng-pipelines.yml \
  --output stage-decompiler/revng-sdk.json
```
