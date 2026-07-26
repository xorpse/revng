use std::fs;
use std::path::Path;

use object::elf::{DT_NEEDED, FileHeader32, FileHeader64};
use object::macho::{MachHeader32, MachHeader64};
use object::read::elf::{Dyn, FileHeader};
use object::read::macho::{LoadCommandVariant, MachHeader};
use object::{Endianness, FileKind};

use crate::error::Error;

pub(crate) struct MachoReferences {
    pub(crate) dylibs: Vec<String>,
    pub(crate) rpaths: Vec<String>,
}

pub(crate) fn macho_references(path: &Path) -> Result<MachoReferences, Error> {
    let data = fs::read(path).map_err(|source| Error::io(path, source))?;
    match FileKind::parse(&*data).map_err(|source| Error::inspect(path, source))? {
        FileKind::MachO64 => references::<MachHeader64<Endianness>>(&data, path),
        FileKind::MachO32 => references::<MachHeader32<Endianness>>(&data, path),
        _ => Err(Error::UnexpectedFormat {
            path: path.to_owned(),
            expected: "Mach-O",
        }),
    }
}

pub(crate) fn elf_needed(path: &Path) -> Result<Vec<String>, Error> {
    let data = fs::read(path).map_err(|source| Error::io(path, source))?;
    match FileKind::parse(&*data).map_err(|source| Error::inspect(path, source))? {
        FileKind::Elf64 => needed::<FileHeader64<Endianness>>(&data, path),
        FileKind::Elf32 => needed::<FileHeader32<Endianness>>(&data, path),
        _ => Err(Error::UnexpectedFormat {
            path: path.to_owned(),
            expected: "ELF",
        }),
    }
}

fn references<H: MachHeader<Endian = Endianness>>(
    data: &[u8],
    path: &Path,
) -> Result<MachoReferences, Error> {
    let inspect = |source| Error::inspect(path, source);
    let header = H::parse(data, 0).map_err(inspect)?;
    let endian = header.endian().map_err(inspect)?;
    let mut commands = header.load_commands(endian, data, 0).map_err(inspect)?;
    let mut dylibs = Vec::new();
    let mut rpaths = Vec::new();
    while let Some(command) = commands.next().map_err(inspect)? {
        match command.variant().map_err(inspect)? {
            LoadCommandVariant::Dylib(dylib) => {
                let name = command.string(endian, dylib.dylib.name).map_err(inspect)?;
                dylibs.push(String::from_utf8_lossy(name).into_owned());
            }
            LoadCommandVariant::Rpath(rpath) => {
                let path = command.string(endian, rpath.path).map_err(inspect)?;
                rpaths.push(String::from_utf8_lossy(path).into_owned());
            }
            _ => {}
        }
    }
    Ok(MachoReferences { dylibs, rpaths })
}

fn needed<H: FileHeader<Endian = Endianness>>(
    data: &[u8],
    path: &Path,
) -> Result<Vec<String>, Error> {
    let inspect = |source| Error::inspect(path, source);
    let header = H::parse(data).map_err(inspect)?;
    let endian = header.endian().map_err(inspect)?;
    let sections = header.sections(endian, data).map_err(inspect)?;
    let Some((entries, index)) = sections.dynamic(endian, data).map_err(inspect)? else {
        return Ok(Vec::new());
    };
    let strings = sections.strings(endian, data, index).map_err(inspect)?;
    let mut names = Vec::new();
    for entry in entries {
        if entry.tag32(endian) == Some(DT_NEEDED) {
            let name = entry.string(endian, strings).map_err(inspect)?;
            names.push(String::from_utf8_lossy(name).into_owned());
        }
    }
    Ok(names)
}
