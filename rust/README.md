# Rust SDK integration

`revng-build` locates a staged revng SDK and its pinned LLVM from build
scripts and emits every compile and link setting consumers need. `revng-sys`
provides the raw PipelineC callback ABI and relays the discovered prefixes to
its dependants. `revng-inkwell` adapts PipelineC's borrowed LLVM handles to
scoped Inkwell `BorrowedModule` views and provides a transactional transform
callback without exposing raw ownership handling to consumers. `revng-fugue`
decompiles fugue-lifted binaries through the revng pipeline.

## Locating the SDK

Consumers point two environment variables at the staged trees:

```sh
export REVNG_SDK=/path/to/staged-revng-sdk     # include/revng, lib/, share/revng/pipelines
export REVNG_LLVM=/path/to/pinned-llvm-install # revng's patched LLVM 16 build
```

`revng-build` derives everything else by layout convention: headers from
`$REVNG_SDK/include` and `llvm-config --includedir`, libraries from
`$REVNG_SDK/lib` (+ `lib/revng/analyses`) and `llvm-config --libdir`,
pipelines from `$REVNG_SDK/share/revng/pipelines`, and the C++ compiler from
`$REVNG_LLVM/bin/clang++` on Linux or Apple `clang++` on macOS. The LLVM major
version is pinned to 16 and validated against `llvm-config`.

Boost and libarchive are system dependencies discovered automatically, in the
usual sys-crate order: `BOOST_INCLUDEDIR`/`BOOST_ROOT` and
`LIBARCHIVE_INCLUDEDIR`/`LIBARCHIVE_ROOT` overrides when set (a set-but-wrong
value is a hard error), pkg-config (libarchive only — boost ships no `.pc`),
the well-known homebrew prefixes (`/opt/homebrew`, `/usr/local`), then
`brew --prefix` as a last resort. When nothing is found no include flag is
emitted and the compiler's default search paths apply — correct on Linux
distributions that install headers under `/usr/include`.

The former `revng-sdk.json` manifest and its generator are gone;
`REVNG_SDK_MANIFEST` is no longer read.

## Consuming from a build script

```toml
[build-dependencies]
revng-build = { path = ".../rust/revng-build" }
```

```rust
fn main() {
    let sdk = revng_build::Sdk::discover().unwrap_or_else(|error| panic!("{error}"));
    sdk.configure_linkage();
}
```

Every crate that links a final artefact calls `configure_linkage()` — Cargo
does not propagate executable linker arguments (RPATHs, constructor-retained
backend and analysis DSOs) from a transitive sys crate. C++ shims are compiled
with `configure_cxx(&mut build)`, which applies the compiler, the required
`-std=c++20 -stdlib=libc++ -fno-rtti` flags, and every include directory to a
`cc::Build` (including the one returned by `cxx_build::bridge`). Pipeline
YAMLs are baked with `emit_pipeline_env("full", "REVNG_FULL_PIPELINE")`.

Direct dependants of `revng-sys` may rely on its `DEP_REVNG_SDK`,
`DEP_REVNG_LLVM`, and `DEP_REVNG_LLVM_MAJOR` metadata inside their build
scripts; everything else discovers through the environment variables.

The supported targets are Linux x86-64 and experimental native macOS AArch64.
macOS builds use revng's pinned LLVM 16 fork and libc++; they support callback
and reference backends but exclude libtcg. See
[`../docs/linux-sdk.md`](../docs/linux-sdk.md) and
[`../docs/macos-sdk.md`](../docs/macos-sdk.md) for staging the SDK trees.

`revng-fugue`'s pipeline tests must run single-threaded:
`cargo test -- --test-threads=1`.
