//! Rust port of `php/bin/bytecode-pack` — byte-compatible with the PHP tool.
//!
//! Wraps a raw `OPD2` blob in a BYTC version-2 container: derives a per-file
//! AES-256-GCM key from the input keying material via HKDF over a random
//! `key_id` salt, encrypts with a random nonce, and writes
//! `header || nonce || tag || ciphertext`. A container packed here verifies
//! under either `bytecode-verify` implementation.

use std::path::{Path, PathBuf};
use std::process::exit;

use bytecode_tools::*;

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

/// Resolve the hex IKM, creating the default vendor key file when nothing is
/// configured — exactly as the PHP tool does (and with the same 0770/0600
/// permissions).
fn resolve_hex_key() -> String {
    let have_env = env_nonempty("BYTECODE_DEK").is_some()
        || env_nonempty("BYTECODE_KEY").is_some()
        || env_nonempty("OPDUMP_KEY").is_some()
        || env_nonempty("BYTECODE_VENDOR_KEY").is_some();
    let vkf = vendor_key_file();

    if !vkf.is_file() && !have_env {
        if let Some(dir) = vkf.parent() {
            if !dir.is_dir() {
                if let Err(e) = std::fs::create_dir_all(dir) {
                    eprintln!("cannot create vendor key directory: {}", dir.display());
                    let _ = e;
                    exit(1);
                }
                set_mode(dir, 0o770);
            }
        }
        let generated = format!("{}\n", hex::encode(random_bytes(32)));
        if std::fs::write(&vkf, generated).is_err() {
            eprintln!("cannot write vendor key file: {}", vkf.display());
            exit(1);
        }
        set_mode(&vkf, 0o600);
    }

    // BYTECODE_DEK ?: BYTECODE_KEY ?: OPDUMP_KEY ?: BYTECODE_VENDOR_KEY ?: fileKey
    resolve_ikm_hex_decrypt()
}

#[cfg(unix)]
fn set_mode(path: &Path, mode: u32) {
    use std::os::unix::fs::PermissionsExt;
    let _ = std::fs::set_permissions(path, std::fs::Permissions::from_mode(mode));
}

#[cfg(not(unix))]
fn set_mode(_path: &Path, _mode: u32) {}

fn main() {
    let argv: Vec<String> = std::env::args().collect();
    let args: Vec<String> = argv[1..].to_vec();

    let is_help = !args.is_empty() && matches!(args[0].as_str(), "-h" | "--help");
    if args.len() != 2 || is_help {
        usage();
        // Matches PHP: ($argc === 2 && help) ? 0 : 2  (argc === 2 => one arg)
        exit(if args.len() == 1 && is_help { 0 } else { 2 });
    }

    let raw_path = PathBuf::from(&args[0]);
    let out_path = PathBuf::from(&args[1]);

    let hex_key = resolve_hex_key();
    if !is_hex64(&hex_key) {
        eprintln!(
            "BYTECODE_DEK/BYTECODE_KEY/OPDUMP_KEY/BYTECODE_VENDOR_KEY must be exactly 64 hex characters"
        );
        exit(1);
    }

    let raw = match std::fs::read(&raw_path) {
        Ok(b) => b,
        Err(_) => {
            eprintln!("cannot read raw blob: {}", raw_path.display());
            exit(1);
        }
    };
    if raw.len() < 4 || &raw[0..4] != b"OPD2" {
        eprintln!("raw blob is not OPD2: {}", raw_path.display());
        exit(1);
    }

    let ikm = hex::decode(&hex_key).unwrap();
    let key_id = random_bytes(16);
    let key = hkdf_sha256(&ikm, 32, b"bytecode-v2", &key_id);
    let nonce = random_bytes(12);
    let php_version_id = php_version_id();

    // header = BYTC | version | php_version_id | backend_id | backend_format
    //          | cipher_id | key_id
    let mut header = Vec::new();
    header.extend_from_slice(b"BYTC");
    put_u32le(&mut header, 2); // version
    put_u32le(&mut header, php_version_id);
    put_lp32(&mut header, BACKEND_PHP.as_bytes());
    put_lp32(&mut header, FORMAT_OPD2.as_bytes());
    put_u32le(&mut header, 1); // cipher_id: AES-256-GCM
    put_lp32(&mut header, &key_id);

    let (ciphertext, tag) = aes256gcm_encrypt(&key, &nonce, &header, &raw).unwrap_or_else(|| {
        eprintln!("AES-256-GCM encryption failed");
        exit(1);
    });

    let mut container = header;
    put_lp32(&mut container, &nonce);
    put_lp32(&mut container, &tag);
    put_u64le(&mut container, ciphertext.len() as u64);
    container.extend_from_slice(&ciphertext);

    if std::fs::write(&out_path, &container).is_err() {
        eprintln!("cannot write container: {}", out_path.display());
        exit(1);
    }
}
