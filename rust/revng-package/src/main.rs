use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Arg, ArgAction, Command, value_parser};

use crate::error::Error;
use crate::package::Packager;

mod error;
mod inspect;
mod package;

fn command() -> Command {
    Command::new("revng-package")
        .about("Assemble a relocatable runtime package from a provisioned revng cache and a consumer binary")
        .arg(
            Arg::new("cache")
                .long("cache")
                .required(true)
                .value_parser(value_parser!(PathBuf))
                .help("Provisioned cache key directory (contains llvm/ and sdk/)"),
        )
        .arg(
            Arg::new("binary")
                .long("binary")
                .required(true)
                .value_parser(value_parser!(PathBuf))
                .help("Consumer binary built with REVNG_BUILD_PORTABLE=1"),
        )
        .arg(
            Arg::new("out")
                .long("out")
                .required(true)
                .value_parser(value_parser!(PathBuf))
                .help("Package directory to create (must be empty or absent)"),
        )
        .arg(
            Arg::new("name")
                .long("name")
                .help("Name for the packaged binary (default: the binary's file name)"),
        )
        .arg(
            Arg::new("no-strip")
                .long("no-strip")
                .action(ArgAction::SetTrue)
                .help("Keep symbols; Linux strips the shipped libraries by default"),
        )
}

fn run() -> Result<(), Error> {
    let matches = command().get_matches();
    let cache = matches
        .get_one::<PathBuf>("cache")
        .expect("cache is required");
    let binary = matches
        .get_one::<PathBuf>("binary")
        .expect("binary is required");
    let out = matches.get_one::<PathBuf>("out").expect("out is required");

    let mut packager = Packager::new(cache.clone(), binary.clone(), out.clone())?;
    if let Some(name) = matches.get_one::<String>("name") {
        packager = packager.with_name(name.clone());
    }
    if matches.get_flag("no-strip") {
        packager = packager.with_strip(false);
    }

    let summary = packager.run()?;
    println!(
        "packaged {} libraries and {} analyses into {}",
        summary.libraries,
        summary.analyses,
        summary.out.display()
    );
    if !summary.bundled.is_empty() {
        println!(
            "bundled {}: {}",
            summary.bundled.len(),
            summary.bundled.join(", ")
        );
    }
    if summary.stripped {
        println!("stripped shipped binaries");
    }
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::FAILURE
        }
    }
}
