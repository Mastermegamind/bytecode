//! Rust port of `php/bin/bytecode-license-keygen` — byte-compatible with the
//! PHP tool. Generates an RSA-4096 keypair for BYTC2 license-mode key wrapping:
//!
//!   <output-dir>/license.key.pem  private key (chmod 600) -- stays with whoever
//!                                 runs the decoded app, never shipped
//!   <output-dir>/license.pub.pem  public key -- BYTECODE_LICENSE_PUBKEY at encode
//!
//! The PHP tool uses ext-openssl (openssl_pkey_new / _export), which emits a
//! PKCS#8 private key and an SPKI public key; `openssl genpkey` + `openssl pkey
//! -pubout` produce exactly those same PEM encodings.

use std::path::Path;
use std::process::exit;

use bytecode_tools::{chmod, fail, openssl_run};

fn main() {
    let argv: Vec<String> = std::env::args().collect();
    let args = &argv[1..];
    let is_help = args.len() == 1 && matches!(args[0].as_str(), "-h" | "--help");
    if args.len() != 1 || is_help {
        eprintln!("Usage: bytecode-license-keygen <output-dir>");
        eprintln!();
        eprintln!("Generates an RSA-4096 keypair for BYTC2 license-mode key wrapping:");
        eprintln!("  <output-dir>/license.key.pem  private key (chmod 600) -- keep with whoever");
        eprintln!("                                runs the decoded app, never ship it with a build");
        eprintln!("  <output-dir>/license.pub.pem  public key -- feed to");
        eprintln!("                                BYTECODE_LICENSE_PUBKEY at encode time");
        exit(if is_help { 0 } else { 2 });
    }

    let out_dir = args[0].trim_end_matches('/').to_string();
    let out_dir_path = Path::new(&out_dir);
    if !out_dir_path.is_dir() && std::fs::create_dir_all(out_dir_path).is_err() && !out_dir_path.is_dir() {
        fail(format!("cannot create output directory: {out_dir}"));
    }

    let private_path = out_dir_path.join("license.key.pem");
    let public_path = out_dir_path.join("license.pub.pem");

    if private_path.exists() || public_path.exists() {
        fail(format!("refusing to overwrite existing key material in {out_dir}"));
    }

    let (code, private_pem, stderr) = openssl_run(
        &["genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:4096"],
        None,
    );
    if code != 0 || !String::from_utf8_lossy(&private_pem).contains("PRIVATE KEY") {
        fail(format!(
            "openssl genpkey (RSA-4096) failed -- is the openssl CLI installed?\n{stderr}"
        ));
    }
    if std::fs::write(&private_path, &private_pem).is_err() {
        fail(format!("cannot write {}", private_path.display()));
    }
    chmod(&private_path, 0o600);

    let (code, public_pem, stderr) = openssl_run(&["pkey", "-pubout"], Some(&private_pem));
    if code != 0 || !String::from_utf8_lossy(&public_pem).contains("PUBLIC KEY") {
        let _ = std::fs::remove_file(&private_path);
        fail(format!("failed to export public key\n{stderr}"));
    }
    if std::fs::write(&public_path, &public_pem).is_err() {
        fail(format!("cannot write {}", public_path.display()));
    }

    println!(
        "wrote {} (keep this with whoever runs the decoded app)",
        private_path.display()
    );
    println!(
        "wrote {} (use with BYTECODE_LICENSE_PUBKEY at encode time)",
        public_path.display()
    );
}
