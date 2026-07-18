use std::collections::BTreeMap;
use std::env;
use std::fmt;
use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

pub const MANIFEST_FILE: &str = "revng-sdk.json";
pub const SCHEMA_VERSION: u32 = 1;

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct Manifest {
    pub schema_version: u32,
    pub target_os: String,
    pub target_arch: String,
    pub llvm_major: u32,
    pub cxx_compiler: PathBuf,
    pub include_dirs: Vec<PathBuf>,
    pub library_dirs: Vec<PathBuf>,
    pub runtime_library_dirs: Vec<PathBuf>,
    pub link_libraries: Vec<String>,
    #[serde(default)]
    pub link_files: Vec<PathBuf>,
    #[serde(default)]
    pub backend_libraries: BTreeMap<String, PathBuf>,
    #[serde(default)]
    pub registry_libraries: Vec<PathBuf>,
    #[serde(default)]
    pub pipelines: BTreeMap<String, PathBuf>,
}

#[derive(Clone, Debug)]
pub struct Sdk {
    manifest_path: PathBuf,
    root: PathBuf,
    manifest: Manifest,
}

#[derive(Debug)]
pub struct Error(String);

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

impl Sdk {
    pub fn discover() -> Result<Self, Error> {
        if let Some(path) = env::var_os("REVNG_SDK_MANIFEST") {
            return Self::load(path);
        }
        if let Some(root) = env::var_os("REVNG_SDK") {
            return Self::load(PathBuf::from(root).join(MANIFEST_FILE));
        }
        Err(Error(format!(
            "set REVNG_SDK to an SDK directory or REVNG_SDK_MANIFEST to {MANIFEST_FILE}"
        )))
    }

    pub fn load(path: impl AsRef<Path>) -> Result<Self, Error> {
        let path = path.as_ref();
        let manifest_path = fs::canonicalize(path).map_err(|error| {
            Error(format!(
                "failed to open revng SDK manifest {}: {error}",
                path.display()
            ))
        })?;
        let contents = fs::read_to_string(&manifest_path).map_err(|error| {
            Error(format!(
                "failed to read revng SDK manifest {}: {error}",
                manifest_path.display()
            ))
        })?;
        let manifest: Manifest = serde_json::from_str(&contents).map_err(|error| {
            Error(format!(
                "invalid revng SDK manifest {}: {error}",
                manifest_path.display()
            ))
        })?;
        if manifest.schema_version != SCHEMA_VERSION {
            return Err(Error(format!(
                "unsupported revng SDK schema {}; expected {SCHEMA_VERSION}",
                manifest.schema_version
            )));
        }
        let root = manifest_path.parent().unwrap().to_owned();
        let sdk = Self {
            manifest_path,
            root,
            manifest,
        };
        sdk.validate_paths()?;
        Ok(sdk)
    }

    pub fn validate_target(&self, target_os: &str, target_arch: &str) -> Result<(), Error> {
        if self.manifest.target_os != target_os || self.manifest.target_arch != target_arch {
            return Err(Error(format!(
                "revng SDK targets {}-{}, but Cargo targets {target_os}-{target_arch}",
                self.manifest.target_os, self.manifest.target_arch
            )));
        }
        Ok(())
    }

    pub fn manifest(&self) -> &Manifest {
        &self.manifest
    }

    pub fn manifest_path(&self) -> &Path {
        &self.manifest_path
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    pub fn resolve(&self, path: &Path) -> PathBuf {
        if path.is_absolute() {
            path.to_owned()
        } else {
            self.root.join(path)
        }
    }

    pub fn compiler(&self) -> PathBuf {
        self.resolve(&self.manifest.cxx_compiler)
    }

    pub fn include_dirs(&self) -> impl Iterator<Item = PathBuf> + '_ {
        self.manifest
            .include_dirs
            .iter()
            .map(|path| self.resolve(path))
    }

    pub fn library_dirs(&self) -> impl Iterator<Item = PathBuf> + '_ {
        self.manifest
            .library_dirs
            .iter()
            .map(|path| self.resolve(path))
    }

    pub fn runtime_library_dirs(&self) -> impl Iterator<Item = PathBuf> + '_ {
        self.manifest
            .runtime_library_dirs
            .iter()
            .map(|path| self.resolve(path))
    }

    pub fn backend(&self, name: &str) -> Option<PathBuf> {
        self.manifest
            .backend_libraries
            .get(name)
            .map(|path| self.resolve(path))
    }

    pub fn pipeline(&self, name: &str) -> Option<PathBuf> {
        self.manifest
            .pipelines
            .get(name)
            .map(|path| self.resolve(path))
    }

    pub fn emit_cargo_link_directives(&self) {
        for directory in self.library_dirs() {
            println!("cargo:rustc-link-search=native={}", directory.display());
        }
        for directory in self.runtime_library_dirs() {
            println!("cargo:rustc-link-search=native={}", directory.display());
        }
        for library in &self.manifest.link_libraries {
            println!("cargo:rustc-link-lib=dylib={library}");
        }

        let retained_libraries = self
            .manifest
            .backend_libraries
            .values()
            .chain(self.manifest.registry_libraries.iter());
        if self.manifest.target_os == "linux"
            && (!self.manifest.backend_libraries.is_empty()
                || !self.manifest.registry_libraries.is_empty())
        {
            println!("cargo:rustc-link-arg=-Wl,--no-as-needed");
        }
        for path in retained_libraries {
            let path = self.resolve(path);
            if self.manifest.target_os == "macos" {
                println!(
                    "cargo:rustc-link-arg=-Wl,-needed_library,{}",
                    path.display()
                );
            } else {
                println!("cargo:rustc-link-arg={}", path.display());
            }
        }
        if self.manifest.target_os == "linux" {
            println!("cargo:rustc-link-arg=-Wl,--disable-new-dtags");
        }
        for path in &self.manifest.link_files {
            println!("cargo:rustc-link-arg={}", self.resolve(path).display());
        }
        for directory in self.runtime_library_dirs() {
            println!("cargo:rustc-link-arg=-Wl,-rpath,{}", directory.display());
        }
    }

    pub fn emit_cargo_rerun_directives(&self) {
        println!("cargo:rerun-if-env-changed=REVNG_SDK");
        println!("cargo:rerun-if-env-changed=REVNG_SDK_MANIFEST");
        println!("cargo:rerun-if-changed={}", self.manifest_path.display());
    }

    fn validate_paths(&self) -> Result<(), Error> {
        let mut required = vec![self.compiler()];
        required.extend(self.include_dirs());
        required.extend(self.library_dirs());
        required.extend(self.runtime_library_dirs());
        required.extend(
            self.manifest
                .link_files
                .iter()
                .map(|path| self.resolve(path)),
        );
        required.extend(
            self.manifest
                .registry_libraries
                .iter()
                .map(|path| self.resolve(path)),
        );
        required.extend(
            self.manifest
                .backend_libraries
                .values()
                .map(|path| self.resolve(path)),
        );
        required.extend(
            self.manifest
                .pipelines
                .values()
                .map(|path| self.resolve(path)),
        );
        if let Some(missing) = required.into_iter().find(|path| !path.exists()) {
            return Err(Error(format!(
                "revng SDK path does not exist: {}",
                missing.display()
            )));
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::{Manifest, Sdk, MANIFEST_FILE, SCHEMA_VERSION};
    use std::collections::BTreeMap;
    use std::fs;
    use std::path::PathBuf;
    use std::time::{SystemTime, UNIX_EPOCH};

    #[test]
    fn manifest_round_trips() {
        let manifest = Manifest {
            schema_version: SCHEMA_VERSION,
            target_os: "macos".into(),
            target_arch: "aarch64".into(),
            llvm_major: 16,
            cxx_compiler: "bin/clang++".into(),
            include_dirs: vec![PathBuf::from("include")],
            library_dirs: vec![PathBuf::from("lib")],
            runtime_library_dirs: vec![PathBuf::from("lib")],
            link_libraries: vec!["revngPipelineC".into()],
            link_files: Vec::new(),
            backend_libraries: BTreeMap::new(),
            registry_libraries: Vec::new(),
            pipelines: BTreeMap::new(),
        };
        let json = serde_json::to_string(&manifest).unwrap();
        let decoded: Manifest = serde_json::from_str(&json).unwrap();
        assert_eq!(decoded.target_os, "macos");
        assert_eq!(decoded.target_arch, "aarch64");
    }

    #[test]
    fn relative_manifest_survives_sdk_move() {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let temporary = std::env::temp_dir().join(format!("revng-sdk-{nonce}"));
        let original = temporary.join("original");
        let moved = temporary.join("moved");
        for directory in ["bin", "include", "lib", "pipelines"] {
            fs::create_dir_all(original.join(directory)).unwrap();
        }
        fs::write(original.join("bin/clang++"), []).unwrap();
        fs::write(original.join("lib/backend.so"), []).unwrap();
        fs::write(original.join("pipelines/lift.yml"), []).unwrap();
        let manifest = Manifest {
            schema_version: SCHEMA_VERSION,
            target_os: "linux".into(),
            target_arch: "x86_64".into(),
            llvm_major: 16,
            cxx_compiler: "bin/clang++".into(),
            include_dirs: vec!["include".into()],
            library_dirs: vec!["lib".into()],
            runtime_library_dirs: vec!["lib".into()],
            link_libraries: vec!["revngPipelineC".into()],
            link_files: Vec::new(),
            backend_libraries: BTreeMap::from([("test".into(), PathBuf::from("lib/backend.so"))]),
            registry_libraries: Vec::new(),
            pipelines: BTreeMap::from([("lift".into(), PathBuf::from("pipelines/lift.yml"))]),
        };
        fs::write(
            original.join(MANIFEST_FILE),
            serde_json::to_vec(&manifest).unwrap(),
        )
        .unwrap();
        fs::rename(&original, &moved).unwrap();
        let sdk = Sdk::load(moved.join(MANIFEST_FILE)).unwrap();
        let canonical_moved = moved.canonicalize().unwrap();
        assert_eq!(sdk.compiler(), canonical_moved.join("bin/clang++"));
        assert_eq!(
            sdk.backend("test"),
            Some(canonical_moved.join("lib/backend.so"))
        );
        fs::remove_dir_all(temporary).unwrap();
    }
}
