use std::collections::{BTreeMap, BTreeSet};
use std::env;
use std::ffi::OsStr;
use std::fs;
use std::os::unix::fs::{PermissionsExt, symlink};
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::error::Error;
use crate::inspect;

const LIBRARY_RUNPATH: &str = "$ORIGIN:$ORIGIN/revng/analyses";
const ANALYSIS_RUNPATH: &str = "$ORIGIN:$ORIGIN/../..";
const BUNDLED_RUNPATH: &str = "$ORIGIN";

const LINUX_SYSTEM_LIBRARIES: [&str; 9] = [
    "libc.so.6",
    "libdl.so.2",
    "libgcc_s.so.1",
    "libm.so.6",
    "libpthread.so.0",
    "libresolv.so.2",
    "librt.so.1",
    "libutil.so.1",
    "linux-vdso.so.1",
];
const LINUX_GENERIC_DIRECTORIES: [&str; 4] = ["/usr/lib64", "/lib64", "/usr/lib", "/lib"];

#[derive(Clone, Copy, PartialEq)]
enum Os {
    Linux,
    MacOs,
}

impl Os {
    fn host() -> Result<Self, Error> {
        match env::consts::OS {
            "linux" => Ok(Os::Linux),
            "macos" => Ok(Os::MacOs),
            other => Err(Error::UnsupportedTarget(other.to_owned())),
        }
    }

    fn is_dynamic_library(self, name: &str) -> bool {
        match self {
            Os::Linux => name.ends_with(".so") || name.contains(".so."),
            Os::MacOs => name.ends_with(".dylib"),
        }
    }
}

struct Counts {
    libraries: usize,
    analyses: usize,
}

#[derive(Debug)]
pub(crate) struct Summary {
    pub(crate) out: PathBuf,
    pub(crate) libraries: usize,
    pub(crate) analyses: usize,
    pub(crate) bundled: Vec<String>,
    pub(crate) stripped: bool,
}

pub(crate) struct Packager {
    cache: PathBuf,
    binary: PathBuf,
    out: PathBuf,
    name: String,
    strip: bool,
    os: Os,
}

impl Packager {
    pub(crate) fn new(cache: PathBuf, binary: PathBuf, out: PathBuf) -> Result<Self, Error> {
        let os = Os::host()?;
        let cache = fs::canonicalize(&cache).unwrap_or(cache);
        let binary = fs::canonicalize(&binary).unwrap_or(binary);
        let name = binary
            .file_name()
            .map(|name| name.to_string_lossy().into_owned())
            .ok_or_else(|| Error::MissingInput(binary.clone()))?;
        Ok(Self {
            cache,
            binary,
            out,
            name,
            strip: os == Os::Linux,
            os,
        })
    }

    pub(crate) fn with_name(mut self, name: String) -> Self {
        self.name = name;
        self
    }

    pub(crate) fn with_strip(mut self, strip: bool) -> Self {
        self.strip = strip && self.os == Os::Linux;
        self
    }

    pub(crate) fn run(&self) -> Result<Summary, Error> {
        self.validate()?;
        self.prepare_out()?;
        self.copy_binary()?;
        let counts = self.copy_libraries()?;
        self.copy_share()?;
        let bundled = match self.os {
            Os::Linux => self.bundle_and_relocate()?,
            Os::MacOs => Vec::new(),
        };
        if self.strip {
            self.strip_tree()?;
        }
        self.verify()?;
        Ok(Summary {
            out: self.out.clone(),
            libraries: counts.libraries,
            analyses: counts.analyses,
            bundled,
            stripped: self.strip,
        })
    }

    fn sdk_lib(&self) -> PathBuf {
        self.cache.join("sdk/lib")
    }

    fn analyses_source(&self) -> PathBuf {
        self.cache.join("sdk/lib/revng/analyses")
    }

    fn llvm_lib(&self) -> PathBuf {
        self.cache.join("llvm/lib")
    }

    fn share_source(&self) -> PathBuf {
        self.cache.join("sdk/share/revng")
    }

    fn out_bin(&self) -> PathBuf {
        self.out.join("bin")
    }

    fn out_lib(&self) -> PathBuf {
        self.out.join("lib")
    }

    fn out_analyses(&self) -> PathBuf {
        self.out.join("lib/revng/analyses")
    }

    fn out_share(&self) -> PathBuf {
        self.out.join("share/revng")
    }

    fn packaged_binary(&self) -> PathBuf {
        self.out_bin().join(&self.name)
    }

    fn validate(&self) -> Result<(), Error> {
        for path in [
            self.binary.clone(),
            self.sdk_lib(),
            self.analyses_source(),
            self.llvm_lib(),
            self.share_source(),
        ] {
            if !path.exists() {
                return Err(Error::MissingInput(path));
            }
        }
        Ok(())
    }

    fn prepare_out(&self) -> Result<(), Error> {
        if self.out.exists()
            && fs::read_dir(&self.out)
                .map_err(|source| Error::io(&self.out, source))?
                .next()
                .is_some()
        {
            return Err(Error::OutputNotEmpty(self.out.clone()));
        }
        for directory in [
            self.out_bin(),
            self.out_lib(),
            self.out_analyses(),
            self.out_share(),
        ] {
            fs::create_dir_all(&directory).map_err(|source| Error::io(&directory, source))?;
        }
        Ok(())
    }

    fn copy_binary(&self) -> Result<(), Error> {
        let destination = self.packaged_binary();
        copy_file(&self.binary, &destination)?;
        let mut permissions = fs::metadata(&destination)
            .map_err(|source| Error::io(&destination, source))?
            .permissions();
        permissions.set_mode(permissions.mode() | 0o755);
        fs::set_permissions(&destination, permissions)
            .map_err(|source| Error::io(&destination, source))
    }

    fn copy_libraries(&self) -> Result<Counts, Error> {
        let out_lib = self.out_lib();
        let mut placed = BTreeMap::new();
        for source in [self.sdk_lib(), self.llvm_lib()] {
            for path in entries(&source)? {
                let Some(name) = file_name(&path) else {
                    continue;
                };
                if !path.is_file() || !self.os.is_dynamic_library(&name) {
                    continue;
                }
                if let Some(first) = placed.insert(name.clone(), path.clone()) {
                    return Err(Error::Collision {
                        name,
                        first,
                        second: path,
                    });
                }
                copy_file(&path, &out_lib.join(&name))?;
            }
        }
        let out_analyses = self.out_analyses();
        let mut analyses = 0;
        for path in entries(&self.analyses_source())? {
            let Some(name) = file_name(&path) else {
                continue;
            };
            if path.is_file() && self.os.is_dynamic_library(&name) {
                copy_file(&path, &out_analyses.join(&name))?;
                analyses += 1;
            }
        }
        Ok(Counts {
            libraries: placed.len(),
            analyses,
        })
    }

    fn copy_share(&self) -> Result<(), Error> {
        let mut stack = vec![(self.share_source(), self.out_share())];
        while let Some((from, to)) = stack.pop() {
            fs::create_dir_all(&to).map_err(|error| Error::io(&to, error))?;
            for path in entries(&from)? {
                let target = to.join(path.file_name().expect("directory entry has a name"));
                let kind = fs::symlink_metadata(&path)
                    .map_err(|error| Error::io(&path, error))?
                    .file_type();
                if kind.is_dir() {
                    stack.push((path, target));
                } else if kind.is_symlink() {
                    let link = fs::read_link(&path).map_err(|error| Error::io(&path, error))?;
                    symlink(&link, &target).map_err(|error| Error::io(&target, error))?;
                } else {
                    copy_file(&path, &target)?;
                }
            }
        }
        Ok(())
    }

    fn bundle_and_relocate(&self) -> Result<Vec<String>, Error> {
        for library in self.packaged_libraries(&self.out_lib())? {
            self.set_runpath(&library, LIBRARY_RUNPATH)?;
        }
        for analysis in self.packaged_libraries(&self.out_analyses())? {
            self.set_runpath(&analysis, ANALYSIS_RUNPATH)?;
        }
        self.bundle_closure()
    }

    fn bundle_closure(&self) -> Result<Vec<String>, Error> {
        let out_lib = self.out_lib();
        let directories = linux_directories();
        let locate = |soname: &str| {
            directories
                .iter()
                .map(|directory| directory.join(soname))
                .find(|candidate| candidate.exists())
                .and_then(|candidate| fs::canonicalize(candidate).ok())
        };
        let mut present = self.packaged_names()?;
        let mut worklist = vec![self.packaged_binary()];
        worklist.extend(self.packaged_libraries(&self.out_lib())?);
        worklist.extend(self.packaged_libraries(&self.out_analyses())?);

        let mut bundled = Vec::new();
        while let Some(object) = worklist.pop() {
            for soname in inspect::elf_needed(&object)? {
                if present.contains(&soname) || is_system_library(&soname) {
                    continue;
                }
                let source = locate(&soname).ok_or_else(|| Error::UnresolvedDependency {
                    library: object.clone(),
                    dependency: soname.clone(),
                })?;
                let destination = out_lib.join(&soname);
                copy_file(&source, &destination)?;
                self.set_runpath(&destination, BUNDLED_RUNPATH)?;
                present.insert(soname.clone());
                bundled.push(soname);
                worklist.push(destination);
            }
        }
        bundled.sort();
        Ok(bundled)
    }

    fn strip_tree(&self) -> Result<(), Error> {
        let binary = self.packaged_binary();
        run_tool("strip", &[binary.as_os_str()], &binary)?;
        for directory in [self.out_lib(), self.out_analyses()] {
            for library in self.packaged_libraries(&directory)? {
                run_tool(
                    "strip",
                    &[OsStr::new("--strip-unneeded"), library.as_os_str()],
                    &library,
                )?;
            }
        }
        Ok(())
    }

    fn verify(&self) -> Result<(), Error> {
        let mut objects = vec![self.packaged_binary()];
        for directory in [self.out_lib(), self.out_analyses()] {
            objects.extend(self.packaged_libraries(&directory)?);
        }
        match self.os {
            Os::Linux => self.verify_linux(&objects),
            Os::MacOs => self.verify_macos(&objects),
        }
    }

    fn verify_macos(&self, objects: &[PathBuf]) -> Result<(), Error> {
        let cache = self.cache.to_string_lossy().into_owned();
        for object in objects {
            let references = inspect::macho_references(object)?;
            let escape = references
                .dylibs
                .into_iter()
                .chain(references.rpaths)
                .find(|reference| reference.contains(&cache));
            if let Some(dependency) = escape {
                return Err(Error::EscapedDependency {
                    library: object.clone(),
                    dependency,
                });
            }
        }
        Ok(())
    }

    fn verify_linux(&self, objects: &[PathBuf]) -> Result<(), Error> {
        let present = self.packaged_names()?;
        for object in objects {
            for soname in inspect::elf_needed(object)? {
                if present.contains(&soname) || is_system_library(&soname) {
                    continue;
                }
                return Err(Error::EscapedDependency {
                    library: object.clone(),
                    dependency: soname,
                });
            }
        }
        Ok(())
    }

    fn packaged_libraries(&self, directory: &Path) -> Result<Vec<PathBuf>, Error> {
        let mut libraries = Vec::new();
        for path in entries(directory)? {
            if path.is_file()
                && file_name(&path).is_some_and(|name| self.os.is_dynamic_library(&name))
            {
                libraries.push(path);
            }
        }
        Ok(libraries)
    }

    fn packaged_names(&self) -> Result<BTreeSet<String>, Error> {
        let mut names = BTreeSet::new();
        for directory in [self.out_lib(), self.out_analyses()] {
            for path in entries(&directory)? {
                if let Some(name) = file_name(&path) {
                    names.insert(name);
                }
            }
        }
        Ok(names)
    }

    fn set_runpath(&self, target: &Path, runpath: &str) -> Result<(), Error> {
        run_tool(
            "patchelf",
            &[
                OsStr::new("--set-rpath"),
                OsStr::new(runpath),
                target.as_os_str(),
            ],
            target,
        )
    }
}

fn copy_file(source: &Path, destination: &Path) -> Result<(), Error> {
    fs::copy(source, destination).map_err(|error| Error::io(destination, error))?;
    Ok(())
}

fn entries(directory: &Path) -> Result<Vec<PathBuf>, Error> {
    let mut paths = Vec::new();
    for entry in fs::read_dir(directory).map_err(|error| Error::io(directory, error))? {
        paths.push(entry.map_err(|error| Error::io(directory, error))?.path());
    }
    paths.sort();
    Ok(paths)
}

fn file_name(path: &Path) -> Option<String> {
    path.file_name()
        .map(|name| name.to_string_lossy().into_owned())
}

fn linux_loader() -> &'static str {
    match env::consts::ARCH {
        "aarch64" => "ld-linux-aarch64.so.1",
        _ => "ld-linux-x86-64.so.2",
    }
}

fn is_system_library(soname: &str) -> bool {
    soname == linux_loader() || LINUX_SYSTEM_LIBRARIES.contains(&soname)
}

fn linux_directories() -> Vec<PathBuf> {
    let triplet = format!("{}-linux-gnu", env::consts::ARCH);
    let mut directories = vec![
        PathBuf::from(format!("/usr/lib/{triplet}")),
        PathBuf::from(format!("/lib/{triplet}")),
    ];
    directories.extend(LINUX_GENERIC_DIRECTORIES.iter().copied().map(PathBuf::from));
    directories
}

fn run_tool(tool: &'static str, args: &[&OsStr], target: &Path) -> Result<(), Error> {
    let status = Command::new(tool)
        .args(args)
        .status()
        .map_err(|source| Error::ToolMissing { tool, source })?;
    if status.success() {
        Ok(())
    } else {
        Err(Error::Tool {
            tool,
            target: target.to_owned(),
        })
    }
}

#[cfg(test)]
mod test {
    use std::fs;
    use std::path::PathBuf;
    use std::time::{SystemTime, UNIX_EPOCH};

    use super::{Error, Os, Packager, is_system_library, linux_directories};

    fn scratch(name: &str) -> PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let root = std::env::temp_dir().join(format!("revng-package-{name}-{nonce}"));
        fs::create_dir_all(&root).unwrap();
        root
    }

    #[test]
    fn system_classification_and_search_dirs_track_the_host_arch() {
        assert!(is_system_library("libc.so.6"));
        assert!(!is_system_library("librevngSupport.so"));
        assert!(!is_system_library("libarchive.so.13"));

        let triplet = format!("{}-linux-gnu", std::env::consts::ARCH);
        let directories = linux_directories();
        assert_eq!(directories[0], PathBuf::from(format!("/usr/lib/{triplet}")));
        assert!(directories.contains(&PathBuf::from("/usr/lib")));
    }

    #[test]
    fn recognises_versioned_shared_libraries() {
        assert!(Os::Linux.is_dynamic_library("libarchive.so.13"));
        assert!(Os::Linux.is_dynamic_library("librevngSupport.so"));
        assert!(!Os::Linux.is_dynamic_library("librevngScopeGraphUtils.a"));
        assert!(Os::MacOs.is_dynamic_library("librevngSupport.dylib"));
        assert!(!Os::MacOs.is_dynamic_library("libLLVMCore.so"));
    }

    #[test]
    fn merging_the_two_library_trees_rejects_a_collision() {
        let os = Os::host().unwrap();
        let extension = if os == Os::MacOs { "dylib" } else { "so" };
        let root = scratch("collision");
        let cache = root.join("cache");
        for relative in [
            format!("sdk/lib/libclash.{extension}"),
            format!("sdk/lib/revng/analyses/librevngA.{extension}"),
            format!("llvm/lib/libclash.{extension}"),
            "sdk/share/revng/keep".to_owned(),
        ] {
            let path = cache.join(relative);
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(&path, b"\x7fbogus").unwrap();
        }
        let binary = root.join("consumer");
        fs::write(&binary, b"binary").unwrap();

        let packager = Packager::new(cache, binary, root.join("out")).unwrap();
        let error = packager.run().unwrap_err();
        assert!(
            matches!(&error, Error::Collision { name, .. } if *name == format!("libclash.{extension}")),
            "expected a collision error, got {error:?}"
        );
        fs::remove_dir_all(&root).unwrap();
    }
}
