# Installed SDK CMake consumer

This project builds the pure-C [`lifter-library.c`](../lifter-library.c)
consumer against an installed revng SDK:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH="/path/to/stage-decompiler;/path/to/revng-llvm-install;/opt/homebrew;/opt/homebrew/opt/libarchive"
cmake --build build
./build/revng_c_consumer reference-x86_64
```

On a Linux SDK containing libtcg, `libtcg` can be selected instead.
