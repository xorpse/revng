#[cxx::bridge(namespace = "revng_rust")]
mod ffi {
    #[derive(Clone, Copy)]
    struct DecodedInstruction {
        address: u64,
        kind: u8,
    }

    extern "Rust" {
        type RustBackend;

        fn lift(
            backend: &mut RustBackend,
            model_yaml: &str,
            binary: &[u8],
            entries: Vec<String>,
            output: usize,
        ) -> String;
    }

    unsafe extern "C++" {
        include!("revng-rust-lifter-example/cxx/include/bridge.h");

        fn run(backend: &mut RustBackend, pipeline_path: &str, backend_name: &str) -> String;
        fn emit_x86_64(output: usize, instructions: &[DecodedInstruction], entry: u64) -> String;
    }
}

struct RustBackend;

fn parse_address(value: &str) -> Result<u64, String> {
    let address = value
        .split_once(':')
        .map_or(value, |(address, _)| address)
        .strip_prefix("0x")
        .ok_or_else(|| format!("invalid revng address: {value}"))?;
    u64::from_str_radix(address, 16).map_err(|error| error.to_string())
}

fn model_entry(model_yaml: &str) -> Option<&str> {
    let suffix = ":Code_x86_64";
    let end = model_yaml.find(suffix)? + suffix.len();
    let start = model_yaml[..end].rfind("0x")?;
    Some(&model_yaml[start..end])
}

fn lift(
    _backend: &mut RustBackend,
    model_yaml: &str,
    binary: &[u8],
    entries: Vec<String>,
    output: usize,
) -> String {
    if !model_yaml.contains("x86_64") {
        return "the example Rust backend only supports x86_64".into();
    }
    let Some(entry_text) = entries
        .first()
        .map(String::as_str)
        .or_else(|| model_entry(model_yaml))
    else {
        return "the Rust backend needs an entry point".into();
    };
    let entry = match parse_address(entry_text) {
        Ok(value) => value,
        Err(error) => return error,
    };

    // This is the architecture-specific backend logic. A real implementation
    // would call a Rust decoder/semantics crate here and produce the same small
    // architecture-independent instruction description for the IR adapter.
    let mut decoded = Vec::new();
    for (offset, opcode) in binary.iter().copied().enumerate() {
        let kind = match opcode {
            0x90 => 0, // x86-64 NOP
            0xc3 => 1, // x86-64 RET
            _ => return format!("unsupported opcode 0x{opcode:02x} at offset {offset}"),
        };
        decoded.push(ffi::DecodedInstruction {
            address: entry + offset as u64,
            kind,
        });
        if kind == 1 {
            break;
        }
    }
    if !decoded.iter().any(|instruction| instruction.kind == 1) {
        return "the example input has no RET".into();
    }

    ffi::emit_x86_64(output, &decoded, entry)
}

fn main() {
    let backend_name = std::env::args().nth(1).unwrap_or_else(|| "rust".to_owned());
    if !matches!(
        backend_name.as_str(),
        "rust" | "reference-x86_64" | "libtcg"
    ) {
        eprintln!("usage: revng-rust-lifter-example [rust|reference-x86_64|libtcg]");
        std::process::exit(2);
    }

    let mut backend = RustBackend;
    let result = ffi::run(&mut backend, env!("REVNG_EXAMPLE_PIPELINE"), &backend_name);
    if !result.is_empty() {
        eprintln!("{result}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::{model_entry, parse_address};

    #[test]
    fn parses_revng_meta_address() {
        assert_eq!(parse_address("0x400000:Code_x86_64").unwrap(), 0x400000);
    }

    #[test]
    fn finds_entry_in_model() {
        assert_eq!(
            model_entry("EntryPoint: 0x400000:Code_x86_64\n"),
            Some("0x400000:Code_x86_64")
        );
    }
}
