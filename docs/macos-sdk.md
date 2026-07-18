# Native macOS AArch64 build

revng can build both its embeddable library SDK and the full decompiler on
Apple Silicon without libtcg. The callback ABI, the `reference-x86_64` backend,
and external C++ or Rust/Inkwell backends are available. libtcg remains a
Linux-only optional backend; it is not required by the decompiler pipeline.

The full build uses pinned upstream revng forks rather than carrying local
copies of LLVM or nanobind:

* `revng/llvm-project` at `c9bb030b3d3baca5b21a8694e7207da713cdf6bb`
* `revng/nanobind` at `a111828dd36d1ce3c8443d2bfc74ac292169a0f3`

Install Xcode command-line tools and the packaged dependencies:

```sh
brew install cmake ninja boost libarchive zstd python@3.13 yq node
python3.13 -m venv .venv313
.venv313/bin/pip install build setuptools wheel PyYAML Jinja2 jsonschema \
  'jsonschema-to-typeddict>=1.4.2,<2.0' black pycparser llvmcpy yachalk \
  requests requests-toolbelt xdg pyelftools \
  cffi ariadne uvicorn python-multipart starlette aiohttp gql psutil pefile \
  python-idb networkx grandiso zstandard click-option-group hypercorn \
  'psycopg[binary,pool]' uvloop websockets marko lit
```

## Build the pinned toolchain

```sh
git clone https://github.com/revng/llvm-project.git revng-llvm
git -C revng-llvm checkout c9bb030b3d3baca5b21a8694e7207da713cdf6bb
cmake -S revng-llvm/llvm -B revng-llvm-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PWD/revng-llvm-install" \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_CXX_STANDARD=20 \
  -DCMAKE_CXX_STANDARD_REQUIRED=ON \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DLLVM_ENABLE_PROJECTS='clang;mlir' \
  -DLLVM_TARGETS_TO_BUILD='AArch64;ARM;Mips;SystemZ;X86' \
  -DBUILD_SHARED_LIBS=ON \
  -DLLVM_BUILD_LLVM_DYLIB=ON \
  -DLLVM_LINK_LLVM_DYLIB=OFF \
  -DLLVM_ENABLE_DUMP=ON \
  -DLLVM_ENABLE_ASSERTIONS=OFF \
  -DLLVM_TOOL_SANCOV_BUILD=OFF \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF
cmake --build revng-llvm-build --parallel
cmake --install revng-llvm-build

git clone https://github.com/revng/nanobind.git revng-nanobind
git -C revng-nanobind checkout a111828dd36d1ce3c8443d2bfc74ac292169a0f3
git -C revng-nanobind submodule update --init --recursive
cmake -S revng-nanobind/standalone -B revng-nanobind-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PWD/revng-nanobind-install" \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_CXX_STANDARD=20 \
  -DCMAKE_CXX_STANDARD_REQUIRED=ON \
  -DPython_EXECUTABLE="$PWD/.venv313/bin/python"
cmake --build revng-nanobind-build --parallel
cmake --install revng-nanobind-build
```

`sancov` is not used by revng, so both native build recipes disable it. This
also avoids an LLVM 16 initializer conversion rejected by Apple Clang 21. If
another consumer requires the tool, enable it after updating the
`SpecialCaseList::createOrDie()` call to pass `ClIgnorelist.getValue()`.

`LLVM_ENABLE_DUMP=ON` is also required for a Release build. revng uses the
fork's diagnostic `llvm::Value::dump()` API, whose definitions are otherwise
omitted from LLVM libraries compiled with `NDEBUG`. For an LLVM build directory
configured before this option was added, repair and reinstall it in place:

```sh
cmake -S revng-llvm/llvm -B revng-llvm-build \
  -DLLVM_ENABLE_DUMP=ON \
  -DLLVM_TOOL_SANCOV_BUILD=OFF
cmake --build revng-llvm-build --parallel
cmake --install revng-llvm-build

grep '^LLVM_ENABLE_DUMP:BOOL=ON$' revng-llvm-build/CMakeCache.txt
nm -gU revng-llvm-install/lib/libLLVMCore.dylib | c++filt | \
  grep 'llvm::Value::dump() const'
```

Set `CMAKE_OSX_DEPLOYMENT_TARGET` consistently on all three builds when the
artifact must run on an older macOS release.

## Build the full decompiler

```sh
export PATH="$PWD/.venv313/bin:$PATH"
cmake -S . -B build-macos-decompiler -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_INSTALL_PREFIX="$PWD/stage-decompiler" \
  -DCMAKE_PREFIX_PATH="$PWD/revng-llvm-install;$PWD/revng-nanobind-install;/opt/homebrew;/opt/homebrew/opt/libarchive;/opt/homebrew/opt/zstd;/opt/homebrew/opt/boost" \
  -DLLVM_DIR="$PWD/revng-llvm-install/lib/cmake/llvm" \
  -DClang_DIR="$PWD/revng-llvm-install/lib/cmake/clang" \
  -DMLIR_DIR="$PWD/revng-llvm-install/lib/cmake/mlir" \
  -DTARGET_CLANG="$PWD/revng-llvm-install/bin/clang" \
  -DPython3_EXECUTABLE="$PWD/.venv313/bin/python" \
  -DREVNG_SDK_BUILD=OFF \
  -DREVNG_BACKEND_LIBTCG=OFF \
  -DREVNG_BUILD_RUNTIME_SUPPORT=OFF \
  -DREVNG_BUNDLE_TOOLCHAIN_RUNTIME=ON
cmake --build build-macos-decompiler --parallel
test -f build-macos-decompiler/lib/revng/analyses/librevngABI.dylib

ctest --test-dir build-macos-decompiler --output-on-failure \
  -R '^(pypeline-cpp-test|pypeline-annotations-test|pypeline-native-test|test_lifterregistry|test_pipelinec_addressspace|test_lifter_library_c_reference|test_referencelifter|test_reference_end_to_end|test_pipeline_c_tracing|test_gzip_tar_fileGenerator|test_type_shrinking)$'
cmake --install build-macos-decompiler
```

The explicit library check prevents an incomplete build from being mistaken
for an install failure: `cmake --install` copies existing artifacts but does
not build missing targets. If it fails, rerun the build without proceeding to
CTest or installation and inspect the first compiler or linker error.

The focused tests above are the native macOS acceptance set. The repository's
complete historical CTest catalog also contains tests with Linux tooling,
ELF, and libtcg assumptions, so an unfiltered `ctest` is not currently a
macOS portability gate. Tests that specifically require libtcg are unavailable
when `REVNG_BACKEND_LIBTCG=OFF`; the reference and callback backend tests do not
require it.

`REVNG_BUNDLE_TOOLCHAIN_RUNTIME=ON` installs the pinned LLVM, Clang, and MLIR
dylibs beside the revng dylibs. Installed binaries use relative `@rpath`
entries, so they no longer depend on the toolchain build directory. Homebrew's
libarchive and zstd remain package dependencies.

The installed decompiler can then be checked with:

```sh
export PATH="$PWD/.venv313/bin:$PWD/stage-decompiler/bin:$PATH"
revng --help
revng2 project dump-pipeline -o /tmp/revng-pipeline.yml
```

## Consumer SDK

Generate the relocatable descriptor used by the Rust examples:

```sh
.venv313/bin/python scripts/generate-revng-sdk-manifest.py \
  --sdk-root stage-decompiler \
  --llvm-dir revng-llvm-install \
  --cxx /usr/bin/clang++ \
  --pipeline address-space=stage-decompiler/share/revng/pipelines/address-space.yml \
  --pipeline full=stage-decompiler/share/revng/pipelines/revng-pipelines.yml \
  --output stage-decompiler/revng-sdk.json
```

Use Apple Clang for consumer-side C++ shims so the macOS SDK is discovered
automatically. The matching pinned LLVM headers are still needed when a custom
backend manipulates LLVM IR directly, as the Inkwell example does.

The pure-C example is in [`../examples/lifter-library.c`](../examples/lifter-library.c),
the full decompiler example is in
[`../examples/decompile-library.c`](../examples/decompile-library.c),
the CMake integration example is in
[`../examples/cmake-lifter`](../examples/cmake-lifter), and the Rust examples
are in [`../examples/rust-lifter`](../examples/rust-lifter) and
[`../examples/rust-inkwell-lifter`](../examples/rust-inkwell-lifter).
See [`embedding-api.md`](embedding-api.md) for the lazy-byte, serialization,
model, backend-discovery, and direct C-emission APIs.

See [`linux-sdk.md`](linux-sdk.md) for the corresponding Linux x86-64 build,
including the optional libtcg backend.
