//! Rust port of `php/bin/bytecode-pack` — byte-compatible with the PHP tool.
//! Thin CLI wrapper over [`bytecode_tools::ops::pack`]; see that function (and
//! the module docs) for the container format and key derivation.

use std::path::PathBuf;
use std::process::exit;

use bytecode_tools::{ops, php_version_id};

fn usage() {
    eprintln!("Usage: bytecode-pack <raw-opd2-blob> <container-output>");
    eprintln!();
    eprintln!("Environment:");
    eprintln!("  BYTECODE_DEK, BYTECODE_KEY, OPDUMP_KEY, or BYTECODE_VENDOR_KEY");
    eprintln!("    64 hex chars / 32 bytes.");
    eprintln!("    Treated as HKDF-SHA256 input keying material (IKM); the actual");
    eprintln!("    AES-256-GCM key is derived per file from a random key_id salt.");
    eprintln!("    BYTECODE_DEK (set internally by bytecode-dump in license mode)");
    eprintln!("    takes priority over BYTECODE_KEY/OPDUMP_KEY.");
}

fn main() {
    let argv: Vec<String> = std::env::args().collect();
    let args: Vec<String> = argv[1..].to_vec();

    let is_help = !args.is_empty() && matches!(args[0].as_str(), "-h" | "--help");
    if args.len() != 2 || is_help {
        usage();
        exit(if args.len() == 1 && is_help { 0 } else { 2 });
    }

    let raw_path = PathBuf::from(&args[0]);
    let out_path = PathBuf::from(&args[1]);

    if let Err(e) = ops::pack(&raw_path, &out_path, php_version_id()) {
        eprintln!("{e}");
        exit(1);
    }
}
