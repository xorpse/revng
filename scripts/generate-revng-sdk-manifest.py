#!/usr/bin/env python3

import argparse
import json
import os
import platform
import sys
from pathlib import Path


def host_os() -> str:
    if sys.platform.startswith("linux"):
        return "linux"
    if sys.platform == "darwin":
        return "macos"
    raise SystemExit(f"unsupported SDK host: {sys.platform}")


def host_arch() -> str:
    value = platform.machine().lower()
    return {"amd64": "x86_64", "arm64": "aarch64"}.get(value, value)


def relative(path: Path, base: Path) -> str:
    return os.path.relpath(path.resolve(), base.resolve())


def require(path: Path, description: str) -> Path:
    path = path.resolve()
    if not path.exists():
        raise SystemExit(f"{description} does not exist: {path}")
    return path


def parse_named_path(value: str) -> tuple[str, Path]:
    try:
        name, path = value.split("=", 1)
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected NAME=PATH") from error
    if not name:
        raise argparse.ArgumentTypeError("pipeline name cannot be empty")
    return name, Path(path)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate a relocatable descriptor for a built revng SDK"
    )
    parser.add_argument("--source-dir", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument(
        "--sdk-root",
        type=Path,
        help="use one installed SDK prefix for both headers and libraries",
    )
    parser.add_argument("--llvm-dir", type=Path, required=True)
    parser.add_argument("--cxx", type=Path, required=True)
    parser.add_argument("--runtime-lib-dir", type=Path, action="append", default=[])
    parser.add_argument("--pipeline", type=parse_named_path, action="append", default=[])
    parser.add_argument("--target-os", choices=["linux", "macos"], default=host_os())
    parser.add_argument("--target-arch", default=host_arch())
    parser.add_argument(
        "--allow-unsupported-target",
        action="store_true",
        help="describe a target other than Linux x86-64 or experimental macOS AArch64",
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    supported_targets = {("linux", "x86_64"), ("macos", "aarch64")}
    if (args.target_os, args.target_arch) not in supported_targets and not args.allow_unsupported_target:
        raise SystemExit(
            "the SDK target is not supported; pass --allow-unsupported-target "
            "only when developing a source port"
        )

    if args.sdk_root is not None:
        if args.source_dir is not None or args.build_dir is not None:
            raise SystemExit("--sdk-root cannot be combined with --source-dir or --build-dir")
        source = build = require(args.sdk_root, "installed revng SDK")
    else:
        if args.source_dir is None or args.build_dir is None:
            raise SystemExit("pass either --sdk-root or both --source-dir and --build-dir")
        source = require(args.source_dir, "revng source directory")
        build = require(args.build_dir, "revng build directory")
    llvm = require(args.llvm_dir, "LLVM directory")
    cxx = require(args.cxx, "C++ compiler")
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    base = output.parent

    source_include = require(source / "include", "revng source include directory")
    generated_include = require(build / "include", "revng generated include directory")
    revng_lib = require(build / "lib", "revng library directory")
    analyses = require(revng_lib / "revng" / "analyses", "revng analyses directory")
    llvm_include = require(llvm / "include", "LLVM include directory")
    llvm_lib = require(llvm / "lib", "LLVM library directory")
    runtime_dirs = [require(path, "runtime library directory") for path in args.runtime_lib_dir]

    extension = ".dylib" if args.target_os == "macos" else ".so"
    backends = {}
    backend_stems = [("reference-x86_64", "librevngLiftReference")]
    if args.target_os == "linux":
        backend_stems.append(("libtcg", "librevngLiftLibTcg"))
    for name, stem in backend_stems:
        candidate = revng_lib / f"{stem}{extension}"
        if candidate.exists():
            backends[name] = relative(candidate, base)

    pipelines = {
        name: relative(require(path, f"pipeline {name}"), base)
        for name, path in args.pipeline
    }
    link_files = []
    if args.target_os == "linux":
        for filename in ("libc++.so", "libc++abi.so"):
            candidate = next(
                (directory / filename for directory in runtime_dirs if (directory / filename).exists()),
                None,
            )
            if candidate is None:
                raise SystemExit(f"Linux SDK runtime is missing {filename}")
            link_files.append(relative(candidate, base))

    component_llvm = (llvm_lib / f"libLLVMCore{extension}").exists()
    llvm_libraries = (
        [
            "LLVMCore",
            "LLVMSupport",
            "LLVMTarget",
            "LLVMExecutionEngine",
            "LLVMAnalysis",
            "LLVMTransformUtils",
            "LLVMScalarOpts",
            "LLVMInstCombine",
            "LLVMPasses",
        ]
        if component_llvm
        else ["LLVM"]
    )
    if not component_llvm and not (llvm_lib / f"libLLVM{extension}").exists():
        raise SystemExit(f"LLVM directory has neither component nor monolithic {extension} libraries")

    # Expose the standard MLIR C API to Rust/C SDK consumers that borrow an
    # rp_mlir_module in a transform callback. RegisterEverything is useful to
    # high-level Rust wrappers, while IR and Transforms cover direct C API use.
    mlir_libraries = [
        name
        for name in ("MLIRCAPIIR", "MLIRCAPITransforms", "MLIRCAPIRegisterEverything")
        if (llvm_lib / f"lib{name}{extension}").exists()
    ]

    # Full decompiler pipelines are assembled through constructor-registered
    # pipes and LLVM passes. Retain their DSOs in consumers even though no
    # ordinary symbol reference pulls them in. Minimal SDK builds intentionally
    # omit Pipebox and therefore do not need this set.
    registry_libraries = []
    pipebox = revng_lib / f"librevngPipebox{extension}"
    if pipebox.exists():
        registry_candidates = [
            pipebox,
            revng_lib / f"librevngFunctionCallIdentification{extension}",
            revng_lib / f"librevngValueMaterializer{extension}",
            *sorted(analyses.glob(f"librevng*{extension}")),
        ]
        registry_libraries = [
            relative(path, base) for path in dict.fromkeys(registry_candidates) if path.exists()
        ]

    manifest = {
        "schema_version": 1,
        "target_os": args.target_os,
        "target_arch": args.target_arch,
        "llvm_major": 16,
        "cxx_compiler": relative(cxx, base),
        "include_dirs": [
            relative(source_include, base),
            relative(generated_include, base),
            relative(llvm_include, base),
        ],
        "library_dirs": [relative(revng_lib, base), relative(analyses, base), relative(llvm_lib, base)],
        "runtime_library_dirs": [
            relative(revng_lib, base),
            relative(analyses, base),
            relative(llvm_lib, base),
            *(relative(path, base) for path in runtime_dirs),
        ],
        "link_libraries": [
            "revngPipelineC",
            "revngSupport",
            "revngModel",
            *llvm_libraries,
            *mlir_libraries,
            "c++",
        ],
        "link_files": link_files,
        "backend_libraries": backends,
        "registry_libraries": registry_libraries,
        "pipelines": pipelines,
    }
    output.write_text(json.dumps(manifest, indent=2) + "\n")
    print(output)


if __name__ == "__main__":
    main()
