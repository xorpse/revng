# Native Linux x86-64 build

revng can build the full decompiler and its embeddable library SDK on Linux.
Linux supports all three backend forms used by the examples:

* the `libtcg` backend built from revng's QEMU fork;
* the small `reference-x86_64` test backend;
* external callback backends, including the Rust/cxx and Rust/Inkwell examples.

These instructions describe a self-contained source build on a Debian or
Ubuntu x86-64 host. They use the same pinned forks as the macOS build, plus the
pinned QEMU fork used by revng's Orchestra configuration:

* `revng/llvm-project` at `c9bb030b3d3baca5b21a8694e7207da713cdf6bb`
* `revng/nanobind` at `a111828dd36d1ce3c8443d2bfc74ac292169a0f3`
* `revng/qemu` at `8035324196ca7f2d63c63deae1b0e38987573789`

The build consistently uses libc++. Do not mix an LLVM build using libstdc++
with revng or consumer shims using libc++; C++ types cross those library
boundaries.

## Host dependencies

Install the native build dependencies. Package names can differ on Linux
distributions other than Debian and Ubuntu.

```sh
sudo apt-get update
sudo apt-get install -y \
  build-essential clang cmake git ninja-build pkg-config \
  python3 python3-dev python3-venv \
  libc++-dev libc++abi-dev \
  libboost-all-dev libarchive-dev libedit-dev libffi-dev \
  libglib2.0-dev libxml2-dev libzstd-dev zlib1g-dev \
  meson nodejs npm cargo rustc

python3 -m venv .venv
.venv/bin/pip install build setuptools wheel PyYAML Jinja2 jsonschema \
  'jsonschema-to-typeddict>=1.4.2,<2.0' black pycparser llvmcpy yachalk \
  requests requests-toolbelt xdg pyelftools \
  cffi ariadne uvicorn python-multipart starlette aiohttp gql psutil pefile \
  python-idb networkx grandiso zstandard click-option-group hypercorn \
  'psycopg[binary,pool]' uvloop websockets marko lit
```

Install the Go-based `yq` command separately if the distribution package does
not provide mikefarah/yq. revng's scripts require its `-o=json` and `-r`
options.

All commands below are run from the root of a revng source checkout. Keep the
toolchain, QEMU staging area, and revng install prefix together if the resulting
tree will be moved to another machine.

```sh
export LLVM_INSTALL="$PWD/revng-llvm-install"
export NANOBIND_INSTALL="$PWD/revng-nanobind-install"
export SDK_ROOT="$PWD/stage-decompiler"
```

## Build the pinned LLVM, Clang, and MLIR fork

The fork uses C++20 language features. `CMAKE_CXX_STANDARD=20` is required;
without it, compilation fails on declarations such as `concept HasName`.

```sh
git clone https://github.com/revng/llvm-project.git revng-llvm
git -C revng-llvm checkout c9bb030b3d3baca5b21a8694e7207da713cdf6bb

cmake -S revng-llvm/llvm -B revng-llvm-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$LLVM_INSTALL" \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_CXX_STANDARD=20 \
  -DCMAKE_CXX_STANDARD_REQUIRED=ON \
  -DLLVM_ENABLE_LIBCXX=ON \
  -DCLANG_DEFAULT_CXX_STDLIB=libc++ \
  -DLLVM_ENABLE_PROJECTS='clang;mlir' \
  -DLLVM_TARGETS_TO_BUILD='AArch64;ARM;Mips;SystemZ;X86' \
  -DBUILD_SHARED_LIBS=ON \
  -DLLVM_BUILD_LLVM_DYLIB=ON \
  -DLLVM_LINK_LLVM_DYLIB=OFF \
  -DLLVM_ENABLE_DUMP=ON \
  -DLLVM_ENABLE_ASSERTIONS=OFF \
  -DLLVM_ENABLE_TERMINFO=OFF \
  -DLLVM_ENABLE_Z3_SOLVER=OFF \
  -DLLVM_TOOL_SANCOV_BUILD=OFF \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF
cmake --build revng-llvm-build --parallel
cmake --install revng-llvm-build
```

`sancov` is not used by revng, so both native build recipes disable it. This
also avoids an LLVM 16 initializer conversion rejected by newer Clang
frontends. If another consumer requires the tool, enable it after updating the
`SpecialCaseList::createOrDie()` call to pass `ClIgnorelist.getValue()`.

Keep `LLVM_ENABLE_DUMP=ON` in Release builds. revng uses the fork's diagnostic
`llvm::Value::dump()` API, whose definitions are omitted from LLVM libraries
compiled with `NDEBUG` unless this option is enabled.

Verify that the installed compiler selects libc++ by default:

```sh
"$LLVM_INSTALL/bin/clang++" -xc++ -std=c++20 -dM -E /dev/null | \
  grep _LIBCPP_VERSION
```

## Build pinned nanobind

Use the installed fork compiler so nanobind and revng use the same C++ standard
library and ABI.

```sh
git clone https://github.com/revng/nanobind.git revng-nanobind
git -C revng-nanobind checkout a111828dd36d1ce3c8443d2bfc74ac292169a0f3
# nanobind vendors required dependencies as Git submodules.
git -C revng-nanobind submodule update --init --recursive

cmake -S revng-nanobind/standalone -B revng-nanobind-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$NANOBIND_INSTALL" \
  -DCMAKE_C_COMPILER="$LLVM_INSTALL/bin/clang" \
  -DCMAKE_CXX_COMPILER="$LLVM_INSTALL/bin/clang++" \
  -DCMAKE_CXX_STANDARD=20 \
  -DCMAKE_CXX_STANDARD_REQUIRED=ON \
  -DPython_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build revng-nanobind-build --parallel
cmake --install revng-nanobind-build
```

## Build and stage libtcg

revng's QEMU fork produces two distinct sets of artifacts:

* `*-libtcg` targets install the runtime `libtcg-<architecture>.so` libraries;
* `*-llvm-helpers` targets install the `libtcg-helpers-<architecture>.bc`
  modules consumed while revng is built.

The helper build must use Clang from the pinned LLVM fork so its bitcode is
compatible with revng's LLVM tooling.

```sh
git clone https://github.com/revng/qemu.git revng-qemu
git -C revng-qemu checkout 8035324196ca7f2d63c63deae1b0e38987573789
git -C revng-qemu submodule update --init --recursive

mkdir -p qemu-build
(
  cd qemu-build
  ../revng-qemu/configure \
    --cc=/usr/bin/clang \
    --cxx=/usr/bin/clang++ \
    --prefix="$SDK_ROOT" \
    --libdir="$SDK_ROOT/lib" \
    --target-list='arm-linux-user,aarch64-linux-user,i386-linux-user,mips-linux-user,mipsel-linux-user,s390x-linux-user,x86_64-linux-user,arm-libtcg,aarch64-libtcg,i386-libtcg,mips-libtcg,mipsel-libtcg,s390x-libtcg,x86_64-libtcg' \
    --disable-werror --disable-docs --disable-plugins --disable-kvm \
    --disable-tools --disable-system --disable-libnfs --disable-vde \
    --disable-gnutls --disable-cap-ng \
    -Dvhost_user=disabled -Dxkbcommon=disabled \
    --extra-cflags='-fPIC -Wno-unused-variable -Wno-unused-function -Wno-unused-result -Wno-unused-but-set-variable'
)
ninja -C qemu-build
ninja -C qemu-build install

mkdir -p qemu-helpers-build
(
  cd qemu-helpers-build
  ../revng-qemu/configure \
    --cc="$LLVM_INSTALL/bin/clang" \
    --cxx="$LLVM_INSTALL/bin/clang++" \
    --prefix="$SDK_ROOT" \
    --libdir="$SDK_ROOT/lib" \
    --target-list='arm-llvm-helpers,aarch64-llvm-helpers,i386-llvm-helpers,mips-llvm-helpers,mipsel-llvm-helpers,s390x-llvm-helpers,x86_64-llvm-helpers' \
    --disable-werror --disable-docs --disable-plugins --disable-kvm \
    --disable-tools --disable-system --disable-libnfs --disable-vde \
    --disable-gnutls --disable-cap-ng \
    -Dvhost_user=disabled -Dxkbcommon=disabled \
    --extra-cflags='-fPIC -Wno-gcc-compat -DGEN_LLVM_HELPERS -O0 -Xclang -disable-O0-optnone -fembed-bitcode -Wno-unused-variable -Wno-unused-function -Wno-unused-result -Wno-unused-but-set-variable'
)
ninja -C qemu-helpers-build
ninja -C qemu-helpers-build install
```

Before configuring revng, verify that both halves are present:

```sh
test -f "$SDK_ROOT/lib/libtcg-x86_64.so"
test -f "$SDK_ROOT/share/libtcg/libtcg-helpers-x86_64.bc"
test -f "$SDK_ROOT/include/qemu/libtcg/libtcg.h"
```

To build Linux without libtcg, skip this section and configure revng with
`-DREVNG_BACKEND_LIBTCG=OFF`. The reference and callback backends remain
available.

## Build the full decompiler

The QEMU headers are already staged under `$SDK_ROOT/include`; add that path
while compiling the libtcg backend.

```sh
export PATH="$PWD/.venv/bin:$PATH"
cmake -S . -B build-linux-decompiler -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$SDK_ROOT" \
  -DCMAKE_C_COMPILER="$LLVM_INSTALL/bin/clang" \
  -DCMAKE_CXX_COMPILER="$LLVM_INSTALL/bin/clang++" \
  -DCMAKE_CXX_FLAGS="-I$SDK_ROOT/include" \
  -DCMAKE_PREFIX_PATH="$LLVM_INSTALL;$NANOBIND_INSTALL;$SDK_ROOT" \
  -DLLVM_DIR="$LLVM_INSTALL/lib/cmake/llvm" \
  -DClang_DIR="$LLVM_INSTALL/lib/cmake/clang" \
  -DMLIR_DIR="$LLVM_INSTALL/lib/cmake/mlir" \
  -DTARGET_CLANG="$LLVM_INSTALL/bin/clang" \
  -DPython3_EXECUTABLE="$PWD/.venv/bin/python" \
  -DREVNG_SDK_BUILD=OFF \
  -DREVNG_BACKEND_LIBTCG=ON \
  -DREVNG_BUILD_RUNTIME_SUPPORT=ON
cmake --build build-linux-decompiler --parallel
ctest --test-dir build-linux-decompiler --output-on-failure
cmake --install build-linux-decompiler
```

For a local non-packaged installation, make the staged revng, LLVM, and libc++
libraries visible when running the CLI:

```sh
CXX_RUNTIME_LIBRARY="$("$LLVM_INSTALL/bin/clang++" \
  -print-file-name=libc++.so)"
CXX_RUNTIME_DIR="$(dirname "$CXX_RUNTIME_LIBRARY")"
export LD_LIBRARY_PATH="$SDK_ROOT/lib:$SDK_ROOT/lib/revng/analyses:$LLVM_INSTALL/lib:$CXX_RUNTIME_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$PWD/.venv/bin:$SDK_ROOT/bin:$PATH"

revng --help
revng2 project dump-pipeline -o /tmp/revng-pipeline.yml
```

Linux binary distributions should bundle the matching LLVM and libc++ shared
libraries and rewrite ELF RUNPATHs during packaging. The build tree and local
SDK descriptor below can use their existing locations directly.

## Consumer SDK

Point the Rust crates and examples at the staged trees:

```sh
export REVNG_SDK="$SDK_ROOT"
export REVNG_LLVM="$LLVM_INSTALL"
```

The build scripts derive everything else by layout convention — including the
libc++ runtime, located through
`"$LLVM_INSTALL/bin/clang++" -print-file-name=libc++.so`. See
[`../rust/README.md`](../rust/README.md) for the full discovery and linkage
contract.

Build and run the pure-C installed-SDK consumer with both built-in backends:

```sh
cmake -S examples/cmake-lifter -B build-linux-c-consumer -G Ninja \
  -DCMAKE_PREFIX_PATH="$SDK_ROOT;$LLVM_INSTALL"
cmake --build build-linux-c-consumer
./build-linux-c-consumer/revng_c_consumer reference-x86_64
./build-linux-c-consumer/revng_c_consumer libtcg
```

Run the Rust/cxx and Rust/Inkwell callback backends:

```sh
cargo run --manifest-path examples/rust-lifter/Cargo.toml -- rust
cargo run --manifest-path examples/rust-inkwell-lifter/Cargo.toml -- inkwell
```

The same examples can select `reference-x86_64` or `libtcg` to exercise the
registered native backends from Rust.

[`embedding-api.md`](embedding-api.md) documents the lazy-byte and optional
serialization lifecycle, model construction, backend discovery, and direct
full-pipeline C APIs. [`../examples/decompile-library.c`](../examples/decompile-library.c)
is the corresponding minimal C program and accepts either built-in backend.
