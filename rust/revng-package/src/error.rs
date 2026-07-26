use std::io;
use std::path::{Path, PathBuf};

#[derive(Debug, thiserror::Error)]
pub(crate) enum Error {
    #[error("{name} exists in both {} and {}", first.display(), second.display())]
    Collision {
        name: String,
        first: PathBuf,
        second: PathBuf,
    },
    #[error("{} needs {dependency}, which resolves outside the package", library.display())]
    EscapedDependency {
        library: PathBuf,
        dependency: String,
    },
    #[error("failed to read {}: {source}", path.display())]
    Inspect {
        path: PathBuf,
        source: object::read::Error,
    },
    #[error("failed to access {}: {source}", path.display())]
    Io { path: PathBuf, source: io::Error },
    #[error("required input does not exist: {}", .0.display())]
    MissingInput(PathBuf),
    #[error("output directory {} is not empty", .0.display())]
    OutputNotEmpty(PathBuf),
    #[error("{tool} failed on {}", target.display())]
    Tool { tool: &'static str, target: PathBuf },
    #[error("{tool} is not available: {source}")]
    ToolMissing {
        tool: &'static str,
        source: io::Error,
    },
    #[error("{} is not a {expected} object", path.display())]
    UnexpectedFormat {
        path: PathBuf,
        expected: &'static str,
    },
    #[error("{} needs {dependency}, which is not installed", library.display())]
    UnresolvedDependency {
        library: PathBuf,
        dependency: String,
    },
    #[error("revng-package runs on linux and macos, not {0}")]
    UnsupportedTarget(String),
}

impl Error {
    pub(crate) fn io(path: impl AsRef<Path>, source: io::Error) -> Self {
        Self::Io {
            path: path.as_ref().to_owned(),
            source,
        }
    }

    pub(crate) fn inspect(path: impl AsRef<Path>, source: object::read::Error) -> Self {
        Self::Inspect {
            path: path.as_ref().to_owned(),
            source,
        }
    }
}
