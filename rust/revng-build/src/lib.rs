use std::env;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::provision::{Cache, MissingPrerequisites};

mod provision;

const CXX_FLAGS: [&str; 3] = ["-std=c++20", "-stdlib=libc++", "-fno-rtti"];
const LLVM_MAJOR: u32 = 16;
const LLVM_COMPONENTS: [&str; 9] = [
    "LLVMCore",
    "LLVMSupport",
    "LLVMTarget",
    "LLVMExecutionEngine",
    "LLVMAnalysis",
    "LLVMTransformUtils",
    "LLVMScalarOpts",
    "LLVMInstCombine",
    "LLVMPasses",
];
const MLIR_LIBRARIES: [&str; 3] = [
    "MLIRCAPIIR",
    "MLIRCAPITransforms",
    "MLIRCAPIRegisterEverything",
];
const BASE_LIBRARIES: [&str; 3] = ["revngPipelineC", "revngSupport", "revngModel"];
const REGISTRY_LIBRARIES: [&str; 2] = ["revngFunctionCallIdentification", "revngValueMaterializer"];
const RUNTIME_LIBRARIES: [&str; 2] = ["libc++.so", "libc++abi.so"];
const DEPENDENCIES: [Dependency; 2] = [
    Dependency {
        formula: "boost",
        includedir: "BOOST_INCLUDEDIR",
        root: "BOOST_ROOT",
        pkg_config: None,
        marker: "boost/version.hpp",
    },
    Dependency {
        formula: "libarchive",
        includedir: "LIBARCHIVE_INCLUDEDIR",
        root: "LIBARCHIVE_ROOT",
        pkg_config: Some("libarchive"),
        marker: "archive.h",
    },
];

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("provisioning step '{step}' failed; see {}", log.display())]
    BuildStep { step: &'static str, log: PathBuf },
    #[error("cached SDK at {} is incomplete; delete the directory to reprovision", .0.display())]
    CorruptCache(PathBuf),
    #[error("failed to download {url}: {source}")]
    Download {
        url: String,
        source: Box<ureq::Error>,
    },
    #[error("failed to access {}: {source}", path.display())]
    Io { path: PathBuf, source: io::Error },
    #[error("revng pins LLVM {LLVM_MAJOR}, but llvm-config reports {found}")]
    LlvmMajor { found: u32 },
    #[error("unexpected llvm-config output: {output}")]
    LlvmVersion { output: String },
    #[error("{variable} does not provide {}", path.display())]
    MissingInclude { variable: String, path: PathBuf },
    #[error("revng SDK path does not exist: {}", .0.display())]
    MissingPath(PathBuf),
    #[error("{} does not provide {name}", compiler.display())]
    MissingRuntime { compiler: PathBuf, name: String },
    #[error("no usable cache directory; set REVNG_BUILD_CACHE or HOME")]
    NoCacheRoot,
    #[error("REVNG_SDK is set but REVNG_LLVM is not; set both or neither")]
    NoLlvm,
    #[error("REVNG_LLVM is set but REVNG_SDK is not; set both or neither")]
    NoSdk,
    #[error("{0}")]
    Preflight(MissingPrerequisites),
    #[error("failed to run {}: {source}", path.display())]
    Toolchain { path: PathBuf, source: io::Error },
    #[error("revng supports linux-x86_64 and macos-aarch64, not {os}-{arch}")]
    UnsupportedTarget { arch: String, os: String },
}

impl Error {
    fn io(path: impl AsRef<Path>, source: io::Error) -> Self {
        Self::Io {
            path: path.as_ref().to_owned(),
            source,
        }
    }

    fn toolchain(path: impl AsRef<Path>, source: io::Error) -> Self {
        Self::Toolchain {
            path: path.as_ref().to_owned(),
            source,
        }
    }
}

struct Dependency {
    formula: &'static str,
    includedir: &'static str,
    root: &'static str,
    pkg_config: Option<&'static str>,
    marker: &'static str,
}

impl Dependency {
    fn provided_by(&self, directory: &Path) -> bool {
        directory.join(self.marker).exists()
    }

    fn resolve(&self) -> Result<Option<PathBuf>, Error> {
        let (variable, directory) = if let Some(directory) = env::var_os(self.includedir) {
            (self.includedir, PathBuf::from(directory))
        } else if let Some(root) = env::var_os(self.root) {
            (self.root, PathBuf::from(root).join("include"))
        } else {
            return Ok(self.discover());
        };
        if self.provided_by(&directory) {
            Ok(Some(directory))
        } else {
            Err(Error::MissingInclude {
                variable: variable.to_owned(),
                path: directory.join(self.marker),
            })
        }
    }

    fn discover(&self) -> Option<PathBuf> {
        self.pkg_config
            .and_then(|name| {
                pkg_config::Config::new()
                    .cargo_metadata(false)
                    .env_metadata(true)
                    .probe(name)
                    .ok()
            })
            .and_then(|library| {
                library
                    .include_paths
                    .into_iter()
                    .find(|directory| self.provided_by(directory))
            })
            .or_else(|| self.wellknown())
            .or_else(|| self.brew_prefix())
    }

    fn wellknown(&self) -> Option<PathBuf> {
        ["/opt/homebrew", "/usr/local"]
            .into_iter()
            .flat_map(|prefix| {
                [
                    PathBuf::from(format!("{prefix}/opt/{}/include", self.formula)),
                    PathBuf::from(format!("{prefix}/include")),
                ]
            })
            .find(|directory| self.provided_by(directory))
    }

    // `brew --prefix` takes seconds; it must stay the last resort.
    fn brew_prefix(&self) -> Option<PathBuf> {
        let output = Command::new("brew")
            .args(["--prefix", self.formula])
            .output()
            .ok()?;
        if !output.status.success() {
            return None;
        }
        let directory =
            PathBuf::from(String::from_utf8_lossy(&output.stdout).trim()).join("include");
        self.provided_by(&directory).then_some(directory)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
enum Os {
    Linux,
    MacOs,
}

impl Os {
    fn extension(self) -> &'static str {
        match self {
            Os::Linux => ".so",
            Os::MacOs => ".dylib",
        }
    }

    fn library(self, stem: &str) -> String {
        format!("lib{stem}{}", self.extension())
    }
}

#[derive(Debug)]
pub struct Sdk {
    prefix: PathBuf,
    llvm: PathBuf,
    llvm_include: PathBuf,
    llvm_lib: PathBuf,
    llvm_major: u32,
    component_llvm: bool,
    link_files: Vec<PathBuf>,
    dependency_includes: Vec<PathBuf>,
    os: Os,
}

impl Sdk {
    pub fn discover() -> Result<Self, Error> {
        let target_os =
            env::var("CARGO_CFG_TARGET_OS").expect("CARGO_CFG_TARGET_OS is set by Cargo");
        let target_arch =
            env::var("CARGO_CFG_TARGET_ARCH").expect("CARGO_CFG_TARGET_ARCH is set by Cargo");
        let os = match (target_os.as_str(), target_arch.as_str()) {
            ("linux", "x86_64") => Os::Linux,
            ("macos", "aarch64") => Os::MacOs,
            _ => {
                return Err(Error::UnsupportedTarget {
                    arch: target_arch,
                    os: target_os,
                });
            }
        };

        let canonicalise =
            |path: PathBuf| fs::canonicalize(&path).map_err(|source| Error::Io { path, source });
        let sdk_variable = env::var_os("DEP_REVNG_SDK").or_else(|| env::var_os("REVNG_SDK"));
        let llvm_variable = env::var_os("DEP_REVNG_LLVM").or_else(|| env::var_os("REVNG_LLVM"));
        let (prefix, llvm) = match (sdk_variable, llvm_variable) {
            (Some(prefix), Some(llvm)) => (PathBuf::from(prefix), PathBuf::from(llvm)),
            (Some(_), None) => return Err(Error::NoLlvm),
            (None, Some(_)) => return Err(Error::NoSdk),
            (None, None) => {
                let cache = Cache::ensure(os)?;
                (cache.sdk(), cache.llvm())
            }
        };
        let prefix = canonicalise(prefix)?;
        let llvm = canonicalise(llvm)?;

        let llvm_config = llvm.join("bin/llvm-config");
        let compiler = Self::compiler_for(os, &llvm);
        for path in [
            prefix.join("include/revng"),
            prefix.join("lib/revng/analyses"),
            prefix.join("lib").join(os.library("revngPipelineC")),
            llvm_config.clone(),
            compiler.clone(),
        ] {
            if !path.exists() {
                return Err(Error::MissingPath(path));
            }
        }

        let output = Command::new(&llvm_config)
            .args(["--version", "--includedir", "--libdir"])
            .output()
            .map_err(|source| Error::Toolchain {
                path: llvm_config,
                source,
            })?;
        if !output.status.success() {
            return Err(Error::LlvmVersion {
                output: String::from_utf8_lossy(&output.stderr).into_owned(),
            });
        }
        let stdout = String::from_utf8_lossy(&output.stdout);
        let mut lines = stdout.lines();
        let (Some(version), Some(includedir), Some(libdir)) =
            (lines.next(), lines.next(), lines.next())
        else {
            return Err(Error::LlvmVersion {
                output: stdout.into_owned(),
            });
        };
        let major = version
            .split('.')
            .next()
            .and_then(|major| major.trim().parse::<u32>().ok())
            .ok_or_else(|| Error::LlvmVersion {
                output: version.to_owned(),
            })?;
        if major != LLVM_MAJOR {
            return Err(Error::LlvmMajor { found: major });
        }
        let llvm_include = PathBuf::from(includedir.trim());
        let llvm_lib = PathBuf::from(libdir.trim());

        let component_llvm = llvm_lib.join(os.library("LLVMCore")).exists();
        if !component_llvm {
            let monolithic = llvm_lib.join(os.library("LLVM"));
            if !monolithic.exists() {
                return Err(Error::MissingPath(monolithic));
            }
        }

        let link_files = match os {
            Os::Linux => Self::runtime_libraries(&compiler)?,
            Os::MacOs => Vec::new(),
        };

        let mut dependency_includes = Vec::new();
        for dependency in &DEPENDENCIES {
            dependency_includes.extend(dependency.resolve()?);
        }

        for variable in ["REVNG_SDK", "REVNG_LLVM", "REVNG_BUILD_CACHE"] {
            println!("cargo::rerun-if-env-changed={variable}");
        }
        for dependency in &DEPENDENCIES {
            println!("cargo::rerun-if-env-changed={}", dependency.includedir);
            println!("cargo::rerun-if-env-changed={}", dependency.root);
        }

        Ok(Self {
            prefix,
            llvm,
            llvm_include,
            llvm_lib,
            llvm_major: major,
            component_llvm,
            link_files,
            dependency_includes,
            os,
        })
    }

    pub fn prefix(&self) -> &Path {
        &self.prefix
    }

    pub fn llvm(&self) -> &Path {
        &self.llvm
    }

    pub fn llvm_major(&self) -> u32 {
        self.llvm_major
    }

    pub fn compiler(&self) -> PathBuf {
        Self::compiler_for(self.os, &self.llvm)
    }

    pub fn configure_linkage(&self) {
        for directive in self.link_directives() {
            println!("{directive}");
        }
    }

    pub fn configure_cxx(&self, build: &mut cc::Build) {
        build
            .compiler(self.compiler())
            .cpp(true)
            .warnings(false)
            .include(self.prefix.join("include"))
            .include(&self.llvm_include);
        for include in &self.dependency_includes {
            build.include(include);
        }
        for flag in CXX_FLAGS {
            build.flag(flag);
        }
    }

    fn compiler_for(os: Os, llvm: &Path) -> PathBuf {
        match os {
            Os::Linux => llvm.join("bin/clang++"),
            Os::MacOs => PathBuf::from("/usr/bin/clang++"),
        }
    }

    fn runtime_libraries(compiler: &Path) -> Result<Vec<PathBuf>, Error> {
        RUNTIME_LIBRARIES
            .into_iter()
            .map(|name| {
                let output = Command::new(compiler)
                    .arg(format!("-print-file-name={name}"))
                    .output()
                    .map_err(|source| Error::Toolchain {
                        path: compiler.to_owned(),
                        source,
                    })?;
                let path = PathBuf::from(String::from_utf8_lossy(&output.stdout).trim());
                if path.is_absolute() && path.exists() {
                    Ok(path)
                } else {
                    Err(Error::MissingRuntime {
                        compiler: compiler.to_owned(),
                        name: name.to_owned(),
                    })
                }
            })
            .collect()
    }

    fn search_directories(&self) -> Vec<PathBuf> {
        let mut directories = vec![
            self.prefix.join("lib"),
            self.prefix.join("lib/revng/analyses"),
            self.llvm_lib.clone(),
        ];
        for file in &self.link_files {
            if let Some(parent) = file.parent()
                && !directories.iter().any(|directory| directory == parent)
            {
                directories.push(parent.to_owned());
            }
        }
        directories
    }

    // Constructor-registered pipes and passes must stay loaded even though no
    // ordinary symbol reference pulls them in.
    fn retained_libraries(&self) -> Vec<PathBuf> {
        let lib = self.prefix.join("lib");
        let mut retained = Vec::new();
        for stem in ["revngLiftReference"].into_iter().chain(REGISTRY_LIBRARIES) {
            let path = lib.join(self.os.library(stem));
            if path.exists() {
                retained.push(path);
            }
        }
        let analyses = self.prefix.join("lib/revng/analyses");
        let mut entries = fs::read_dir(&analyses)
            .expect("analyses directory validated at discovery")
            .filter_map(|entry| entry.ok().map(|entry| entry.path()))
            .filter(|path| {
                path.file_name()
                    .and_then(|name| name.to_str())
                    .is_some_and(|name| {
                        name.starts_with("librevng") && name.ends_with(self.os.extension())
                    })
            })
            .collect::<Vec<_>>();
        entries.sort();
        retained.extend(entries);
        retained
    }

    fn link_directives(&self) -> Vec<String> {
        let mut directives = Vec::new();
        let search_directories = self.search_directories();
        for directory in &search_directories {
            directives.push(format!(
                "cargo::rustc-link-search=native={}",
                directory.display()
            ));
        }
        let llvm_libraries = if self.component_llvm {
            &LLVM_COMPONENTS[..]
        } else {
            &["LLVM"]
        };
        let mlir_libraries = MLIR_LIBRARIES
            .into_iter()
            .filter(|library| self.llvm_lib.join(self.os.library(library)).exists());
        for library in BASE_LIBRARIES
            .into_iter()
            .chain(llvm_libraries.iter().copied())
            .chain(mlir_libraries)
            .chain(["c++"])
        {
            directives.push(format!("cargo::rustc-link-lib=dylib={library}"));
        }

        let retained = self.retained_libraries();
        if self.os == Os::Linux && !retained.is_empty() {
            directives.push("cargo::rustc-link-arg=-Wl,--no-as-needed".to_owned());
        }
        for path in &retained {
            directives.push(match self.os {
                Os::Linux => format!("cargo::rustc-link-arg={}", path.display()),
                Os::MacOs => {
                    format!(
                        "cargo::rustc-link-arg=-Wl,-needed_library,{}",
                        path.display()
                    )
                }
            });
        }
        if self.os == Os::Linux {
            directives.push("cargo::rustc-link-arg=-Wl,--disable-new-dtags".to_owned());
        }
        for path in &self.link_files {
            directives.push(format!("cargo::rustc-link-arg={}", path.display()));
        }
        for directory in &search_directories {
            directives.push(format!(
                "cargo::rustc-link-arg=-Wl,-rpath,{}",
                directory.display()
            ));
        }
        directives
    }
}

#[cfg(test)]
mod test {
    use std::env;
    use std::fs;
    use std::os::unix::fs::PermissionsExt;
    use std::path::PathBuf;
    use std::time::{SystemTime, UNIX_EPOCH};

    use super::{Error, Os, Sdk};

    struct Fixture {
        root: PathBuf,
    }

    impl Fixture {
        fn new(name: &str) -> Self {
            let nonce = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos();
            let root = env::temp_dir().join(format!("revng-build-{name}-{nonce}"));
            fs::create_dir_all(&root).unwrap();
            Self { root }
        }

        fn file(&self, path: &str) -> PathBuf {
            let path = self.root.join(path);
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(&path, []).unwrap();
            path
        }

        fn sdk(&self, os: Os, component_llvm: bool, link_files: Vec<PathBuf>) -> Sdk {
            Sdk {
                prefix: self.root.join("sdk"),
                llvm: self.root.join("llvm"),
                llvm_include: self.root.join("llvm/include"),
                llvm_lib: self.root.join("llvm/lib"),
                llvm_major: 16,
                component_llvm,
                link_files,
                dependency_includes: Vec::new(),
                os,
            }
        }
    }

    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.root);
        }
    }

    fn stub_compiler(fixture: &Fixture) -> PathBuf {
        let path = fixture.root.join("llvm/bin/clang++");
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(
            &path,
            "#!/bin/sh\necho \"$(cd \"$(dirname \"$0\")/../..\" && pwd)/runtime/${1#-print-file-name=}\"\n",
        )
        .unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o755)).unwrap();
        path
    }

    #[test]
    fn macos_link_directives() {
        let fixture = Fixture::new("macos");
        for path in [
            "sdk/lib/librevngPipelineC.dylib",
            "sdk/lib/librevngLiftReference.dylib",
            "sdk/lib/librevngFunctionCallIdentification.dylib",
            "sdk/lib/librevngValueMaterializer.dylib",
            "sdk/lib/revng/analyses/librevngB.dylib",
            "sdk/lib/revng/analyses/librevngA.dylib",
            "llvm/lib/libLLVMCore.dylib",
            "llvm/lib/libMLIRCAPIIR.dylib",
        ] {
            fixture.file(path);
        }
        let sdk = fixture.sdk(Os::MacOs, true, Vec::new());
        let directives = sdk.link_directives();

        let root = fixture.root.display();
        let searches = directives
            .iter()
            .filter(|directive| directive.starts_with("cargo::rustc-link-search="))
            .collect::<Vec<_>>();
        assert_eq!(
            searches,
            [
                &format!("cargo::rustc-link-search=native={root}/sdk/lib"),
                &format!("cargo::rustc-link-search=native={root}/sdk/lib/revng/analyses"),
                &format!("cargo::rustc-link-search=native={root}/llvm/lib"),
            ]
        );
        assert!(directives.contains(&"cargo::rustc-link-lib=dylib=LLVMCore".to_owned()));
        assert!(directives.contains(&"cargo::rustc-link-lib=dylib=MLIRCAPIIR".to_owned()));
        assert!(directives.contains(&"cargo::rustc-link-lib=dylib=c++".to_owned()));
        assert!(!directives.contains(&"cargo::rustc-link-lib=dylib=MLIRCAPITransforms".to_owned()));

        let retained = directives
            .iter()
            .filter(|directive| directive.contains("-needed_library"))
            .collect::<Vec<_>>();
        assert_eq!(
            retained,
            [
                &format!(
                    "cargo::rustc-link-arg=-Wl,-needed_library,{root}/sdk/lib/librevngLiftReference.dylib"
                ),
                &format!(
                    "cargo::rustc-link-arg=-Wl,-needed_library,{root}/sdk/lib/librevngFunctionCallIdentification.dylib"
                ),
                &format!(
                    "cargo::rustc-link-arg=-Wl,-needed_library,{root}/sdk/lib/librevngValueMaterializer.dylib"
                ),
                &format!(
                    "cargo::rustc-link-arg=-Wl,-needed_library,{root}/sdk/lib/revng/analyses/librevngA.dylib"
                ),
                &format!(
                    "cargo::rustc-link-arg=-Wl,-needed_library,{root}/sdk/lib/revng/analyses/librevngB.dylib"
                ),
            ]
        );
        assert!(
            !directives
                .iter()
                .any(|directive| directive.contains("--no-as-needed"))
        );
        assert!(
            !directives
                .iter()
                .any(|directive| directive.contains("--disable-new-dtags"))
        );
        assert!(directives.contains(&format!("cargo::rustc-link-arg=-Wl,-rpath,{root}/llvm/lib")));
    }

    #[test]
    fn linux_link_directives() {
        let fixture = Fixture::new("linux");
        for path in [
            "sdk/lib/librevngPipelineC.so",
            "sdk/lib/librevngLiftReference.so",
            "sdk/lib/librevngFunctionCallIdentification.so",
            "sdk/lib/librevngValueMaterializer.so",
            "sdk/lib/revng/analyses/librevngA.so",
            "llvm/lib/libLLVM.so",
        ] {
            fixture.file(path);
        }
        let link_files = vec![
            fixture.file("runtime/libc++.so"),
            fixture.file("runtime/libc++abi.so"),
        ];
        let sdk = fixture.sdk(Os::Linux, false, link_files);
        let directives = sdk.link_directives();

        let root = fixture.root.display();
        assert!(directives.contains(&"cargo::rustc-link-lib=dylib=LLVM".to_owned()));
        assert!(!directives.contains(&"cargo::rustc-link-lib=dylib=LLVMCore".to_owned()));

        let arguments = directives
            .iter()
            .filter(|directive| directive.starts_with("cargo::rustc-link-arg="))
            .collect::<Vec<_>>();
        assert_eq!(
            arguments,
            [
                &"cargo::rustc-link-arg=-Wl,--no-as-needed".to_owned(),
                &format!("cargo::rustc-link-arg={root}/sdk/lib/librevngLiftReference.so"),
                &format!(
                    "cargo::rustc-link-arg={root}/sdk/lib/librevngFunctionCallIdentification.so"
                ),
                &format!("cargo::rustc-link-arg={root}/sdk/lib/librevngValueMaterializer.so"),
                &format!("cargo::rustc-link-arg={root}/sdk/lib/revng/analyses/librevngA.so"),
                &"cargo::rustc-link-arg=-Wl,--disable-new-dtags".to_owned(),
                &format!("cargo::rustc-link-arg={root}/runtime/libc++.so"),
                &format!("cargo::rustc-link-arg={root}/runtime/libc++abi.so"),
                &format!("cargo::rustc-link-arg=-Wl,-rpath,{root}/sdk/lib"),
                &format!("cargo::rustc-link-arg=-Wl,-rpath,{root}/sdk/lib/revng/analyses"),
                &format!("cargo::rustc-link-arg=-Wl,-rpath,{root}/llvm/lib"),
                &format!("cargo::rustc-link-arg=-Wl,-rpath,{root}/runtime"),
            ]
        );
    }

    #[test]
    fn runtime_libraries_resolve_through_the_toolchain() {
        let fixture = Fixture::new("runtime");
        let compiler = stub_compiler(&fixture);
        fixture.file("runtime/libc++.so");
        fixture.file("runtime/libc++abi.so");

        let libraries = Sdk::runtime_libraries(&compiler).unwrap();
        assert_eq!(
            libraries,
            [
                fixture.root.join("runtime/libc++.so"),
                fixture.root.join("runtime/libc++abi.so"),
            ]
        );

        fs::remove_file(fixture.root.join("runtime/libc++abi.so")).unwrap();
        assert!(matches!(
            Sdk::runtime_libraries(&compiler),
            Err(Error::MissingRuntime { name, .. }) if name == "libc++abi.so"
        ));
    }
}
