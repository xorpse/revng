# Rust SDK integration

`revng-build` locates a staged revng SDK and its pinned LLVM from build
scripts and emits every compile and link setting consumers need. `revng-sys`
provides the raw PipelineC callback ABI and relays the discovered prefixes to
its dependants. `revng-inkwell` adapts PipelineC's borrowed LLVM handles to
scoped Inkwell `BorrowedModule` views and provides a transactional transform
callback without exposing raw ownership handling to consumers. `revng-fugue`
decompiles fugue-lifted binaries through the revng pipeline.

## Locating the SDK

Two personas:

**Developers** point two environment variables at working trees:

```sh
export REVNG_SDK=/path/to/staged-revng-sdk     # include/revng, lib/, share/revng/pipelines
export REVNG_LLVM=/path/to/pinned-llvm-install # revng's patched LLVM 16 build
```

**Everyone else sets nothing.** When both variables are absent, `revng-build`
provisions the SDK itself, gmp-mpfr-sys style: it downloads pinned source
tarballs of the revng fork, its LLVM fork, and nanobind, builds them with
CMake/Ninja, and installs into a version-keyed cache outside `target/`:

```
${REVNG_BUILD_CACHE:-<platform cache dir>/revng-build}/<target>/<revng12>-<llvm12>/{llvm,sdk}
```

The platform cache dir is `~/Library/Caches` on macOS and
`${XDG_CACHE_HOME:-~/.cache}` on Linux. Provisioning emits `cargo:warning`
progress lines naming the build log; later builds and other projects reuse the
cache. `cargo clean` never touches it; delete key directories to reclaim
space. Concurrent builds serialise on a file lock.

Provisioning prerequisites (checked up front, one aggregate error with install
hints): `cmake`, `ninja`, `python3` on `PATH`, the system `clang++`, and
boost/libarchive/zstd headers (Linux additionally libc++ and sqlite3 headers).
`python3` remains a runtime requirement — `librevngPipebox` links the system
`libpython`.

For docker/CI, either pre-bake the cache as an image layer (`RUN cargo build`
once during image construction) or mount a volume at `REVNG_BUILD_CACHE` — the
key path is fully deterministic from the pins. A populated
`<key>/{llvm,sdk}` pair is relocatable (resource discovery is relative to the
loaded libraries) and tars up directly as a distribution artefact:
`tar -C <key> -czf revng-sdk.tar.gz llvm sdk`; unpacking it into a cache key
directory and touching `<key>/provisioned` is equivalent to having built it.

Setting exactly one of the two variables is an error, not a fallback.

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

The supported targets are Linux (x86-64, with AArch64 enabled but not yet
validated) and experimental native macOS AArch64.
macOS builds use revng's pinned LLVM 16 fork and libc++; the Rust story
supports the callback and reference backends and excludes libtcg on both
targets. See [`../docs/linux-sdk.md`](../docs/linux-sdk.md) and
[`../docs/macos-sdk.md`](../docs/macos-sdk.md) for staging the SDK trees by
hand (the developer flow the automatic provisioning replicates).

`revng-fugue`'s pipeline tests must run single-threaded:
`cargo test -- --test-threads=1`.

## Building a runtime package

The SDK is the build-time kit; a shipped binary needs only a small relocatable
runtime tree. Build the consumer with `REVNG_BUILD_PORTABLE=1` so `revng-build`
emits package-relative rpaths (`@loader_path/../lib` on macOS, `$ORIGIN/../lib`
on Linux) in place of the absolute cache paths, then run `revng-package`:

```sh
REVNG_BUILD_PORTABLE=1 cargo build --release --bin revng-fugue
cargo run --release --manifest-path ../revng-package/Cargo.toml -- \
  --cache "${REVNG_BUILD_CACHE:-<platform cache>/revng-build}/<target>/<revng12>-<llvm12>" \
  --binary target/release/revng-fugue \
  --out ./package
```

The result is a self-contained tree resolved entirely by relative rpaths — no
launcher, no environment, no SDK:

```
package/
  bin/revng-fugue
  lib/                  revng + LLVM + MLIR + clang dylibs, merged
    revng/analyses/     analysis dylibs
  share/revng/          pipelines, abi, headers, helper-list.csv
```

`./bin/revng-fugue` then runs from any location. On macOS the tree is a pure
copy of the cache dylibs — the binary is born portable, nothing is edited, and
the third-party `libarchive`/`zstd` stay homebrew references (a dev/test
convenience). On Linux the tool additionally bundles the third-party chain,
rewrites rpaths with `patchelf`, and strips the shipped libraries, producing an
artefact that runs in a clean container. The natural production shape is a
multi-stage docker build whose runtime stage copies only the package.
