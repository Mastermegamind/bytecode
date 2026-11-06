//! Rust port of `php/bin/bytecode-vendor-keygen` — byte-compatible with the PHP
//! tool. Generates the vendor Ed25519 signing keypair used to seal encoded
//! packages, writing `vendor.sign.key.pem` (private, chmod 600) and
//! `vendor.sign.pub.pem` (public trust anchor), and printing the raw 32-byte
//! public key as 64 hex for `--with-opdump-vendor-pubkey`.
//!
//! Ed25519 is a one-shot signature over the raw message with no prehash, which
//! PHP's openssl_sign() cannot express — so, like the PHP tool, this shells out
//! to the `openssl` CLI.

use std::path::Path;
use std::process::exit;

use bytecode_tools::{chmod, fail, openssl_run as run};

fn main() {
    let argv: Vec<String> = std::env::args().collect();
    let args = &argv[1..];
    let is_help = args.len() == 1 && matches!(args[0].as_str(), "-h" | "--help");
    if args.len() != 1 || is_help {
        eprintln!("Usage: bytecode-vendor-keygen <output-dir>");
        eprintln!();
        eprintln!("Generates the vendor Ed25519 signing keypair for BYTC2 package sealing:");
        eprintln!("  <output-dir>/vendor.sign.key.pem  private key (chmod 600) -- keep secret,");
        eprintln!("                                    sign builds with BYTECODE_VENDOR_SIGN_KEY");
        eprintln!("  <output-dir>/vendor.sign.pub.pem  public key -- the loader trust anchor");
        eprintln!();
        eprintln!("Also prints the 64-hex raw public key for --with-opdump-vendor-pubkey.");
        exit(if is_help { 0 } else { 2 });
    }

    let out_dir = args[0].trim_end_matches('/').to_string();
    let out_dir_path = Path::new(&out_dir);
    if !out_dir_path.is_dir() && std::fs::create_dir_all(out_dir_path).is_err() && !out_dir_path.is_dir() {
        fail(format!("cannot create output directory: {out_dir}"));
    }

    let private_path = out_dir_path.join("vendor.sign.key.pem");
    let public_path = out_dir_path.join("vendor.sign.pub.pem");

    if private_path.exists() || public_path.exists() {
        fail(format!("refusing to overwrite existing key material in {out_dir}"));
    }

    let (code, private_pem, stderr) = run(&["genpkey", "-algorithm", "ed25519"], None);
    if code != 0 || !String::from_utf8_lossy(&private_pem).contains("PRIVATE KEY") {
        fail(format!(
            "openssl genpkey (ed25519) failed -- is the openssl CLI installed and Ed25519-capable?\n{stderr}"
        ));
    }
    if std::fs::write(&private_path, &private_pem).is_err() {
        fail(format!("cannot write {}", private_path.display()));
    }
    chmod(&private_path, 0o600);

    let (code, public_pem, stderr) = run(&["pkey", "-pubout"], Some(&private_pem));
    if code != 0 || !String::from_utf8_lossy(&public_pem).contains("PUBLIC KEY") {
        let _ = std::fs::remove_file(&private_path);
        fail(format!("openssl pkey -pubout failed\n{stderr}"));
    }
    if std::fs::write(&public_path, &public_pem).is_err() {
        let _ = std::fs::remove_file(&private_path);
        fail(format!("cannot write {}", public_path.display()));
    }

    // The Ed25519 SubjectPublicKeyInfo DER is a fixed 44 bytes: a 12-byte
    // algorithm-identifier prefix followed by the 32-byte key.
    let (code, public_der, stderr) =
        run(&["pkey", "-pubout", "-outform", "DER"], Some(&private_pem));
    if code != 0 || public_der.len() != 44 {
        fail(format!("could not extract raw Ed25519 public key\n{stderr}"));
    }
    let raw_hex = hex::encode(&public_der[12..44]);

    println!(
        "wrote {} (KEEP SECRET -- sign builds via BYTECODE_VENDOR_SIGN_KEY)",
        private_path.display()
    );
    println!(
        "wrote {} (loader trust anchor -- OPDUMP_VENDOR_PUBKEY_FILE)",
        public_path.display()
    );
    println!();
    println!("Compile this raw public key into your loader for the strongest anchor:");
    println!("  ./configure --with-opdump-vendor-pubkey={raw_hex} ...");
}
