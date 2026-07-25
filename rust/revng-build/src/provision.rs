use std::env;
use std::ffi::{OsStr, OsString};
use std::fmt;
use std::fs::{self, File};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use fd_lock::RwLock;
use flate2::read::GzDecoder;
use tar::Archive;

use crate::{DEPENDENCIES, Dependency, Error, Os};

const REVNG: Source = Source {
    repository: "xorpse/revng",
    commit: "d7121d9b5d9e48bbf58c31f280f5680390532bee",
};
const LLVM: Source = Source {
    repository: "revng/llvm-project",
    commit: "c9bb030b3d3baca5b21a8694e7207da713cdf6bb",
};
const PYTHON_REQUIREMENTS: [&str; 5] = [
    "black==26.5.1",
    "jinja2==3.1.6",
    "jsonschema==4.26.0",
    "pycparser==3.0",
    "PyYAML==6.0.3",
];
const ZSTD: Dependency = Dependency {
    formula: "zstd",
    includedir: "ZSTD_INCLUDEDIR",
    root: "ZSTD_ROOT",
    pkg_config: Some("libzstd"),
    marker: "zstd.h",
};
const TOOLS: [Prerequisite; 3] = [
    Prerequisite {
        name: "cmake",
        brew: Some("brew install cmake"),
        apt: Some("apt-get install cmake"),
    },
    Prerequisite {
        name: "ninja",
        brew: Some("brew install ninja"),
        apt: Some("apt-get install ninja-build"),
    },
    Prerequisite {
        name: "python3",
        brew: Some("brew install python@3.13"),
        apt: Some("apt-get install python3 python3-dev python3-venv"),
    },
];

struct Source {
    repository: &'static str,
    commit: &'static str,
}

impl Source {
    fn url(&self) -> String {
        format!(
            "https://codeload.github.com/{}/tar.gz/{}",
            self.repository, self.commit
        )
    }

    fn directory(&self) -> String {
        let name = self
            .repository
            .rsplit('/')
            .next()
            .expect("repository has an owner/name form");
        format!("{name}-{}", self.commit)
    }
}

#[derive(Clone, Copy, Debug)]
struct Prerequisite {
    name: &'static str,
    brew: Option<&'static str>,
    apt: Option<&'static str>,
}

impl Prerequisite {
    fn hint(&self, os: Os) -> Option<&'static str> {
        match os {
            Os::Linux => self.apt,
            Os::MacOs => self.brew,
        }
    }
}

#[derive(Debug)]
pub struct MissingPrerequisites {
    missing: Vec<Prerequisite>,
    os: Os,
}

impl fmt::Display for MissingPrerequisites {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "provisioning the revng SDK needs")?;
        for prerequisite in &self.missing {
            write!(formatter, "\n  {}", prerequisite.name)?;
            if let Some(hint) = prerequisite.hint(self.os) {
                write!(formatter, " ({hint})")?;
            }
        }
        Ok(())
    }
}

pub(crate) struct Cache {
    key: PathBuf,
}

impl Cache {
    pub(crate) fn ensure(os: Os) -> Result<Self, Error> {
        let cache = Self::resolve(os)?;
        if cache.complete()? {
            return Ok(cache);
        }
        preflight(os)?;

        fs::create_dir_all(cache.key.parent().expect("key path has a parent"))
            .map_err(|source| Error::io(&cache.key, source))?;
        let lock_file =
            File::create(cache.lock()).map_err(|source| Error::io(cache.lock(), source))?;
        let mut lock = RwLock::new(lock_file);
        let guard = lock
            .write()
            .map_err(|source| Error::io(cache.lock(), source))?;
        if cache.complete()? {
            return Ok(cache);
        }

        for directory in [cache.llvm(), cache.sdk(), cache.scratch()] {
            if directory.exists() {
                fs::remove_dir_all(&directory).map_err(|source| Error::io(&directory, source))?;
            }
        }
        fs::create_dir_all(cache.scratch()).map_err(|source| Error::io(cache.scratch(), source))?;

        cache.fetch_sources()?;
        let prefixes = dependency_prefixes(os)?;
        for step in cache.steps(os, &prefixes) {
            step.run(&cache)?;
        }

        fs::remove_dir_all(cache.scratch()).map_err(|source| Error::io(cache.scratch(), source))?;
        fs::write(cache.marker(), REVNG.commit)
            .map_err(|source| Error::io(cache.marker(), source))?;
        drop(guard);
        Ok(cache)
    }

    fn resolve(os: Os) -> Result<Self, Error> {
        let root = match env::var_os("REVNG_BUILD_CACHE") {
            Some(root) => PathBuf::from(root),
            None => default_root(os).ok_or(Error::NoCacheRoot)?,
        };
        let target = env::var("TARGET").expect("TARGET is set by Cargo");
        Ok(Self {
            key: key_path(&root, &target),
        })
    }

    pub(crate) fn llvm(&self) -> PathBuf {
        self.key.join("llvm")
    }

    pub(crate) fn sdk(&self) -> PathBuf {
        self.key.join("sdk")
    }

    fn scratch(&self) -> PathBuf {
        self.key.join("scratch")
    }

    fn log(&self) -> PathBuf {
        self.key.join("build.log")
    }

    fn marker(&self) -> PathBuf {
        self.key.join("provisioned")
    }

    fn lock(&self) -> PathBuf {
        self.key.with_extension("lock")
    }

    fn complete(&self) -> Result<bool, Error> {
        if !self.marker().exists() {
            return Ok(false);
        }
        let llvm_config = self.llvm().join("bin/llvm-config");
        if llvm_config.exists() && self.sdk().join("lib").exists() {
            Ok(true)
        } else {
            Err(Error::CorruptCache(self.key.clone()))
        }
    }

    fn fetch_sources(&self) -> Result<(), Error> {
        let scratch = self.scratch();
        for origin in [&REVNG, &LLVM] {
            let url = origin.url();
            let response = ureq::get(&url).call().map_err(|source| Error::Download {
                url: url.clone(),
                source: Box::new(source),
            })?;
            let reader = response.into_body().into_reader();
            Archive::new(GzDecoder::new(reader))
                .unpack(&scratch)
                .map_err(|source| Error::io(&scratch, source))?;
        }
        Ok(())
    }

    fn steps(&self, os: Os, dependency_prefixes: &[PathBuf]) -> Vec<Step> {
        let scratch = self.scratch();
        let venv = scratch.join("venv");
        let venv_python = venv.join("bin/python");
        let llvm_prefix = self.llvm();
        let expect_compiler = |tool: &str| {
            bootstrap_compiler(os, tool).expect("bootstrap compiler validated during preflight")
        };
        let bootstrap_c = expect_compiler("clang");
        let bootstrap_cxx = expect_compiler("clang++");
        let (compiler_c, compiler_cxx) = match os {
            Os::Linux => (
                llvm_prefix.join("bin/clang"),
                llvm_prefix.join("bin/clang++"),
            ),
            Os::MacOs => (bootstrap_c.clone(), bootstrap_cxx.clone()),
        };

        let cmake_common = |step: Step| {
            let step = step
                .define("CMAKE_BUILD_TYPE", "Release")
                .define("CMAKE_CXX_STANDARD", "20")
                .define("CMAKE_CXX_STANDARD_REQUIRED", "ON");
            if os == Os::MacOs {
                step.define("CMAKE_OSX_ARCHITECTURES", "arm64")
            } else {
                step
            }
        };

        let mut configure_llvm = cmake_common(
            Step::new("configure-llvm", "cmake")
                .arg("-S")
                .arg(scratch.join(LLVM.directory()).join("llvm"))
                .arg("-B")
                .arg(scratch.join("llvm-build"))
                .arg("-G")
                .arg("Ninja")
                .define("CMAKE_INSTALL_PREFIX", &llvm_prefix)
                .define("CMAKE_C_COMPILER", &bootstrap_c)
                .define("CMAKE_CXX_COMPILER", &bootstrap_cxx)
                .define("LLVM_ENABLE_PROJECTS", "clang;mlir")
                .define("LLVM_TARGETS_TO_BUILD", "AArch64;ARM;Mips;SystemZ;X86")
                .define("BUILD_SHARED_LIBS", "ON")
                .define("LLVM_BUILD_LLVM_DYLIB", "ON")
                .define("LLVM_LINK_LLVM_DYLIB", "OFF")
                .define("LLVM_ENABLE_DUMP", "ON")
                .define("LLVM_ENABLE_BINDINGS", "OFF")
                .define("LLVM_ENABLE_ASSERTIONS", "OFF")
                .define("LLVM_TOOL_SANCOV_BUILD", "OFF")
                .define("LLVM_INCLUDE_TESTS", "OFF")
                .define("LLVM_INCLUDE_EXAMPLES", "OFF")
                .define("LLVM_INCLUDE_BENCHMARKS", "OFF"),
        );
        if os == Os::Linux {
            configure_llvm = configure_llvm
                .define("LLVM_ENABLE_LIBCXX", "ON")
                .define("CLANG_DEFAULT_CXX_STDLIB", "libc++")
                .define("LLVM_ENABLE_TERMINFO", "OFF")
                .define("LLVM_ENABLE_Z3_SOLVER", "OFF");
        }

        let prefix_path = [&llvm_prefix]
            .into_iter()
            .chain(dependency_prefixes)
            .map(|path| path.display().to_string())
            .collect::<Vec<String>>()
            .join(";");

        let configure_revng = cmake_common(
            Step::new("configure-revng", "cmake")
                .arg("-S")
                .arg(scratch.join(REVNG.directory()))
                .arg("-B")
                .arg(scratch.join("revng-build"))
                .arg("-G")
                .arg("Ninja")
                .define("CMAKE_INSTALL_PREFIX", self.sdk())
                .define("CMAKE_C_COMPILER", &compiler_c)
                .define("CMAKE_CXX_COMPILER", &compiler_cxx)
                .define("CMAKE_PREFIX_PATH", &prefix_path)
                .define("LLVM_DIR", llvm_prefix.join("lib/cmake/llvm"))
                .define("Clang_DIR", llvm_prefix.join("lib/cmake/clang"))
                .define("MLIR_DIR", llvm_prefix.join("lib/cmake/mlir"))
                .define("Python3_EXECUTABLE", &venv_python)
                .define("REVNG_SDK_BUILD", "ON")
                .define("REVNG_BACKEND_LIBTCG", "OFF")
                .define("REVNG_BUILD_RUNTIME_SUPPORT", "OFF")
                .define("REVNG_BUNDLE_TOOLCHAIN_RUNTIME", "OFF"),
        );

        let mut pip_install = Step::new("python-requirements", venv.join("bin/pip"))
            .arg("install")
            .arg("--quiet");
        for requirement in PYTHON_REQUIREMENTS {
            pip_install = pip_install.arg(requirement);
        }

        vec![
            Step::new("venv", "python3")
                .arg("-m")
                .arg("venv")
                .arg(&venv),
            pip_install,
            configure_llvm,
            Step::new("build-llvm", "cmake")
                .arg("--build")
                .arg(scratch.join("llvm-build"))
                .arg("--parallel"),
            Step::new("install-llvm", "cmake")
                .arg("--install")
                .arg(scratch.join("llvm-build")),
            configure_revng,
            Step::new("build-revng", "cmake")
                .arg("--build")
                .arg(scratch.join("revng-build"))
                .arg("--parallel"),
            Step::new("install-revng", "cmake")
                .arg("--install")
                .arg(scratch.join("revng-build")),
        ]
    }
}

fn default_root(os: Os) -> Option<PathBuf> {
    let home = env::var_os("HOME")?;
    let base = match os {
        Os::Linux => match env::var_os("XDG_CACHE_HOME") {
            Some(cache) => PathBuf::from(cache),
            None => PathBuf::from(&home).join(".cache"),
        },
        Os::MacOs => PathBuf::from(&home).join("Library/Caches"),
    };
    Some(base.join("revng-build"))
}

fn key_path(root: &Path, target: &str) -> PathBuf {
    root.join(target)
        .join(format!("{}-{}", &REVNG.commit[..12], &LLVM.commit[..12]))
}

fn preflight(os: Os) -> Result<(), Error> {
    let path = env::var_os("PATH").unwrap_or_default();
    let mut missing = missing_tools(&path)
        .into_iter()
        .copied()
        .collect::<Vec<Prerequisite>>();
    let compiler = bootstrap_compiler(os, "clang++");
    if compiler.is_none() || bootstrap_compiler(os, "clang").is_none() {
        missing.push(Prerequisite {
            name: "clang++",
            brew: Some("xcode-select --install"),
            apt: Some("apt-get install clang"),
        });
    }
    for dependency in DEPENDENCIES.iter().chain([&ZSTD]) {
        let found =
            dependency.resolve()?.is_some() || dependency.provided_by(Path::new("/usr/include"));
        if !found {
            missing.push(header_prerequisite(dependency));
        }
    }
    if os == Os::Linux {
        let libcxx = compiler.is_some_and(|compiler| {
            Command::new(compiler)
                .arg("-print-file-name=libc++.so")
                .output()
                .map(|output| {
                    Path::new(String::from_utf8_lossy(&output.stdout).trim()).is_absolute()
                })
                .unwrap_or(false)
        });
        if !libcxx {
            missing.push(Prerequisite {
                name: "libc++",
                brew: None,
                apt: Some("apt-get install libc++-dev libc++abi-dev"),
            });
        }
        if !Path::new("/usr/include/sqlite3.h").exists() {
            missing.push(Prerequisite {
                name: "sqlite3 headers",
                brew: None,
                apt: Some("apt-get install libsqlite3-dev"),
            });
        }
    }
    if missing.is_empty() {
        Ok(())
    } else {
        Err(Error::Preflight(MissingPrerequisites { missing, os }))
    }
}

fn header_prerequisite(dependency: &Dependency) -> Prerequisite {
    match dependency.formula {
        "boost" => Prerequisite {
            name: "boost headers",
            brew: Some("brew install boost"),
            apt: Some("apt-get install libboost-all-dev"),
        },
        "libarchive" => Prerequisite {
            name: "libarchive headers",
            brew: Some("brew install libarchive"),
            apt: Some("apt-get install libarchive-dev"),
        },
        "zstd" => Prerequisite {
            name: "zstd headers",
            brew: Some("brew install zstd"),
            apt: Some("apt-get install libzstd-dev"),
        },
        formula => unreachable!("no preflight hint for dependency '{formula}'"),
    }
}

fn missing_tools(path: &OsStr) -> Vec<&'static Prerequisite> {
    TOOLS
        .iter()
        .filter(|tool| lookup(path, tool.name).is_none())
        .collect()
}

fn lookup(path: &OsStr, tool: &str) -> Option<PathBuf> {
    env::split_paths(path)
        .map(|directory| directory.join(tool))
        .find(|candidate| candidate.is_file())
}

fn bootstrap_compiler(os: Os, tool: &str) -> Option<PathBuf> {
    match os {
        Os::Linux => lookup(&env::var_os("PATH").unwrap_or_default(), tool),
        Os::MacOs => {
            let apple = PathBuf::from("/usr/bin").join(tool);
            apple.exists().then_some(apple)
        }
    }
}

fn dependency_prefixes(os: Os) -> Result<Vec<PathBuf>, Error> {
    if os == Os::Linux {
        return Ok(Vec::new());
    }
    let mut prefixes = Vec::new();
    for dependency in DEPENDENCIES.iter().chain([&ZSTD]) {
        if let Some(include) = dependency.resolve()?
            && let Some(prefix) = include.parent()
        {
            prefixes.push(prefix.to_owned());
        }
    }
    Ok(prefixes)
}

struct Step {
    name: &'static str,
    program: PathBuf,
    args: Vec<OsString>,
}

impl Step {
    fn new(name: &'static str, program: impl Into<PathBuf>) -> Self {
        Self {
            name,
            program: program.into(),
            args: Vec::new(),
        }
    }

    fn arg(mut self, argument: impl Into<OsString>) -> Self {
        self.args.push(argument.into());
        self
    }

    fn define(self, variable: &str, value: impl AsRef<OsStr>) -> Self {
        let mut argument = OsString::from(format!("-D{variable}="));
        argument.push(value.as_ref());
        self.arg(argument)
    }

    fn run(&self, cache: &Cache) -> Result<(), Error> {
        let open_log = || {
            File::options()
                .create(true)
                .append(true)
                .open(cache.log())
                .map_err(|source| Error::io(cache.log(), source))
        };
        let path = {
            let mut entries = vec![cache.scratch().join("venv/bin")];
            if let Some(existing) = env::var_os("PATH") {
                entries.extend(env::split_paths(&existing));
            }
            env::join_paths(entries).expect("PATH entries are valid")
        };
        let status = Command::new(&self.program)
            .args(&self.args)
            .env("PATH", path)
            .stdin(Stdio::null())
            .stdout(open_log()?)
            .stderr(open_log()?)
            .status()
            .map_err(|source| Error::toolchain(&self.program, source))?;
        if status.success() {
            Ok(())
        } else {
            Err(Error::BuildStep {
                step: self.name,
                log: cache.log(),
            })
        }
    }
}

#[cfg(test)]
mod test {
    use std::env;
    use std::ffi::OsString;
    use std::fs;
    use std::path::{Path, PathBuf};
    use std::time::{SystemTime, UNIX_EPOCH};

    use super::{Cache, key_path, missing_tools};
    use crate::Os;

    fn argument_string(arguments: &[OsString]) -> String {
        arguments
            .iter()
            .map(|argument| argument.to_string_lossy().into_owned())
            .collect::<Vec<String>>()
            .join(" ")
    }

    #[test]
    fn cache_key_is_deterministic() {
        assert_eq!(
            key_path(Path::new("/cache"), "aarch64-apple-darwin"),
            PathBuf::from("/cache/aarch64-apple-darwin/d7121d9b5d9e-c9bb030b3d3b")
        );
    }

    #[test]
    fn step_tables_guard_load_bearing_flags() {
        let cache = Cache {
            key: PathBuf::from("/cache/target/key"),
        };
        for os in [Os::Linux, Os::MacOs] {
            let steps = cache.steps(os, &[]);
            let names = steps.iter().map(|step| step.name).collect::<Vec<_>>();
            assert_eq!(
                names,
                [
                    "venv",
                    "python-requirements",
                    "configure-llvm",
                    "build-llvm",
                    "install-llvm",
                    "configure-revng",
                    "build-revng",
                    "install-revng",
                ]
            );
            let llvm = argument_string(&steps[2].args);
            assert!(llvm.contains("-DLLVM_ENABLE_DUMP=ON"));
            assert!(llvm.contains("-DLLVM_ENABLE_BINDINGS=OFF"));
            assert!(llvm.contains("-DCMAKE_INSTALL_PREFIX=/cache/target/key/llvm"));
            let revng = argument_string(&steps[5].args);
            assert!(revng.contains("-DREVNG_SDK_BUILD=ON"));
            assert!(revng.contains("-DREVNG_BACKEND_LIBTCG=OFF"));
            assert!(revng.contains("-DREVNG_BUNDLE_TOOLCHAIN_RUNTIME=OFF"));
            assert!(revng.contains("-DCMAKE_INSTALL_PREFIX=/cache/target/key/sdk"));
            match os {
                Os::Linux => {
                    assert!(llvm.contains("-DLLVM_ENABLE_LIBCXX=ON"));
                    assert!(
                        revng.contains("-DCMAKE_CXX_COMPILER=/cache/target/key/llvm/bin/clang++")
                    );
                    assert!(!llvm.contains("-DCMAKE_OSX_ARCHITECTURES=arm64"));
                }
                Os::MacOs => {
                    assert!(revng.contains("-DCMAKE_CXX_COMPILER=/usr/bin/clang++"));
                    assert!(llvm.contains("-DCMAKE_OSX_ARCHITECTURES=arm64"));
                }
            }
        }
    }

    #[test]
    fn missing_tools_reports_absent_prerequisites() {
        let missing = missing_tools(&OsString::from("/nonexistent-path-entry"));
        let names = missing.iter().map(|tool| tool.name).collect::<Vec<_>>();
        assert_eq!(names, ["cmake", "ninja", "python3"]);

        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let stubs = env::temp_dir().join(format!("revng-build-tools-{nonce}"));
        fs::create_dir_all(&stubs).unwrap();
        for tool in ["cmake", "ninja", "python3"] {
            fs::write(stubs.join(tool), []).unwrap();
        }
        assert!(missing_tools(stubs.as_os_str()).is_empty());
        fs::remove_dir_all(stubs).unwrap();
    }
}
