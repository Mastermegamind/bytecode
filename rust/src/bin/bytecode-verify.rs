//! Rust port of `php/bin/bytecode-verify` — byte-compatible, same checks,
//! same success output, same exit codes. The PHP script hands off to this
//! binary when it is present; otherwise the PHP implementation runs.

use std::path::{Path, PathBuf};
use std::process::exit;

use base64::Engine;
use bytecode_tools::ops::{self, VerifyOne};
use bytecode_tools::*;
use serde_json::Value;

fn usage_err() -> ! {
    eprintln!("Usage: bytecode-verify [--decrypt-test] <manifest.json|encoded-file>");
    exit(2);
}

/// Verify one container via the shared `ops` surface, exiting with the matching
/// message on failure (CLI behaviour). The structural checks, decrypt-test and
/// error strings all live in `bytecode_tools::ops::verify_one`.
fn verify_container(path: &Path, decrypt_test: bool) -> VerifyOne {
    match ops::verify_one(path, decrypt_test, php_version_id()) {
        Ok(r) => r,
        Err(e) => fail(e),
    }
}

/// JSON value -> string using the same rules as PHP's `(string)` cast, used by
/// the signed seal policy fields.
fn php_string(v: Option<&Value>) -> String {
    match v {
        None | Some(Value::Null) => String::new(),
        Some(Value::String(s)) => s.clone(),
        Some(Value::Number(n)) => n.to_string(),
        Some(Value::Bool(true)) => "1".to_string(),
        Some(Value::Bool(false)) => String::new(),
        Some(Value::Array(_)) | Some(Value::Object(_)) => "Array".to_string(),
    }
}

fn seal_policy_message(seal: &Value) -> String {
    const KEYS: [&str; 10] = [
        "machine_id",
        "domains",
        "ips",
        "hostnames",
        "fingerprints",
        "expires_at",
        "license_id",
        "activation_token",
        "revocation_url",
        "unlock_policy",
    ];
    let mut msg = String::from("bytecode-seal-policy-v1\n");
    for key in KEYS {
        let value = php_string(seal.get(key));
        let shown = if value.is_empty() { "-" } else { &value };
        msg.push_str(key);
        msg.push(':');
        msg.push_str(shown);
        msg.push('\n');
    }
    msg
}

fn read_text(path: &Path) -> String {
    std::fs::read_to_string(path).unwrap_or_else(|_| fail(format!("cannot read {}", path.display())))
}

fn is_file(p: &Path) -> bool {
    p.is_file()
}

fn main() {
    let argv: Vec<String> = std::env::args().collect();
    let mut args: Vec<String> = argv[1..].to_vec();

    if args.is_empty() || matches!(args[0].as_str(), "-h" | "--help") {
        eprintln!("Usage: bytecode-verify [--decrypt-test] <manifest.json|encoded-file>");
        eprintln!();
        eprintln!("Validates BYTC headers, manifest hashes, sizes, map entries, and optionally decrypts every container.");
        exit(if args.is_empty() { 2 } else { 0 });
    }

    let mut decrypt_test = false;
    while !args.is_empty() && args[0].starts_with('-') {
        let arg = args.remove(0);
        if arg == "--decrypt-test" {
            decrypt_test = true;
            continue;
        }
        eprintln!("unknown option: {arg}");
        exit(2);
    }
    if args.len() != 1 {
        usage_err();
    }

    let target = PathBuf::from(&args[0]);
    let mut files: Vec<PathBuf> = Vec::new();

    let manifest_path = if target.is_dir() {
        target.join("bytecode.manifest.json")
    } else {
        target.clone()
    };

    let is_manifest = is_file(&manifest_path)
        && manifest_path
            .to_string_lossy()
            .to_lowercase()
            .ends_with(".json");

    if is_manifest {
        run_manifest(&manifest_path, decrypt_test, &mut files);
    } else {
        verify_container(&target, decrypt_test);
        files.push(target.clone());
    }

    println!(
        "verified {} file(s) metadata/hash/map consistency",
        files.len()
    );
    if decrypt_test {
        println!("decrypt-test: PASS");
    }
}

fn run_manifest(manifest_path: &Path, decrypt_test: bool, files: &mut Vec<PathBuf>) {
    let manifest_text = read_text(manifest_path);
    let manifest: Value = serde_json::from_str(&manifest_text)
        .unwrap_or_else(|_| fail(format!("invalid manifest: {}", manifest_path.display())));
    if manifest.get("format").and_then(Value::as_str) != Some("bytecode-manifest-v1") {
        fail(format!("invalid manifest: {}", manifest_path.display()));
    }
    let base_dir = manifest_path
        .parent()
        .map(Path::to_path_buf)
        .unwrap_or_else(|| PathBuf::from("."));
    let map_name = manifest
        .get("map")
        .and_then(Value::as_str)
        .unwrap_or("bytecode.map");
    let map_path = base_dir.join(map_name);

    // Parse the map: output filename -> source path, and confirm each exists.
    let mut map_outputs: std::collections::HashMap<String, String> = std::collections::HashMap::new();
    if is_file(&map_path) {
        let map_text = read_text(&map_path);
        for line in map_text.lines() {
            if line.is_empty() {
                continue;
            }
            if line.starts_with('#') {
                continue;
            }
            let parts: Vec<&str> = line.splitn(2, '\t').collect();
            if parts.len() != 2 || parts[0].is_empty() || parts[1].is_empty() {
                fail(format!("invalid map line in {}", map_path.display()));
            }
            map_outputs.insert(parts[1].to_string(), parts[0].to_string());
            let mapped = base_dir.join(parts[1]);
            if !is_file(&mapped) {
                fail(format!(
                    "{}: mapped container missing: {}",
                    map_path.display(),
                    parts[1]
                ));
            }
        }
    } else {
        fail(format!("manifest map missing: {}", map_path.display()));
    }

    let empty: Vec<Value> = Vec::new();
    let entries = manifest
        .get("files")
        .and_then(Value::as_array)
        .unwrap_or(&empty);
    for entry in entries {
        let output = entry.get("output").and_then(Value::as_str).unwrap_or("");
        let backend = entry
            .get("backend")
            .and_then(Value::as_str)
            .unwrap_or("php-zend-opdump");
        if backend == BACKEND_PHP && !map_outputs.contains_key(output) {
            fail(format!(
                "{}: missing output for manifest entry {}",
                map_path.display(),
                output
            ));
        }
        let path = base_dir.join(output);
        let result = verify_container(&path, decrypt_test);

        let entry_backend = entry.get("backend").and_then(Value::as_str).unwrap_or("");
        if !entry_backend.is_empty() && entry_backend != result.backend {
            fail(format!("{}: manifest backend mismatch", path.display()));
        }
        let expected_format = match entry.get("backend_format").and_then(Value::as_str) {
            Some(f) => f.to_string(),
            None => {
                if backend == BACKEND_PHP {
                    FORMAT_OPD2.to_string()
                } else {
                    String::new()
                }
            }
        };
        if !expected_format.is_empty() && expected_format != result.backend_format {
            fail(format!("{}: manifest backend_format mismatch", path.display()));
        }
        if entry.get("sha256").and_then(Value::as_str).unwrap_or("") != result.sha256 {
            fail(format!("{}: manifest sha256 mismatch", path.display()));
        }
        if entry.get("size").and_then(Value::as_u64).unwrap_or(0) != result.size {
            fail(format!("{}: manifest size mismatch", path.display()));
        }
        let _ = result.payload_size; // used only via decrypt_test's own failures
        files.push(path);
    }

    verify_manifest_signature(manifest_path, &manifest_text, &map_path, &base_dir);
    verify_seal(manifest_path, &map_path, &base_dir);
}

fn verify_manifest_signature(
    _manifest_path: &Path,
    manifest_text: &str,
    map_path: &Path,
    base_dir: &Path,
) {
    let sig_path = base_dir.join("bytecode.manifest.sig");
    if !is_file(&sig_path) {
        fail(format!("manifest signature missing: {}", sig_path.display()));
    }
    let sig = read_text(&sig_path).trim().to_string();
    if sig.len() != 64 || !sig.bytes().all(|b| b.is_ascii_hexdigit() && !b.is_ascii_uppercase()) {
        fail(format!(
            "{}: malformed signature (expected 64 hex chars)",
            sig_path.display()
        ));
    }
    // Full cryptographic check only when the same IKM the loader uses is
    // available; otherwise this stays a hex-shape check (matches the PHP note).
    let ikm_hex = resolve_ikm_hex_manifest();
    if is_hex64(&ikm_hex) {
        let ikm = hex::decode(&ikm_hex).unwrap();
        let sig_key = hkdf_sha256(&ikm, 32, b"bytecode-manifest-auth", b"");
        let map_text = read_text(map_path);
        let mut signed = Vec::new();
        signed.extend_from_slice(manifest_text.as_bytes());
        signed.push(0u8);
        signed.extend_from_slice(map_text.as_bytes());
        let expected = hmac_sha256_hex(&sig_key, &signed);
        if !ct_eq(&expected, &sig) {
            fail(format!(
                "{}: signature mismatch (wrong key, or manifest/map tampered)",
                sig_path.display()
            ));
        }
    }
}

fn verify_seal(manifest_path: &Path, map_path: &Path, base_dir: &Path) {
    let seal_path = base_dir.join("bytecode.seal.json");
    if !is_file(&seal_path) {
        return;
    }
    let seal: Value = serde_json::from_str(&read_text(&seal_path))
        .unwrap_or_else(|_| fail(format!("invalid bytecode.seal.json: {}", seal_path.display())));
    if seal.get("format").and_then(Value::as_str) != Some("bytecode-seal-v1")
        || seal.get("alg").and_then(Value::as_str) != Some("Ed25519")
    {
        fail(format!("invalid bytecode.seal.json: {}", seal_path.display()));
    }
    let manifest_sha = sha256_file_hex(manifest_path).unwrap();
    let map_sha = sha256_file_hex(map_path).unwrap();
    if seal.get("manifest_sha256").and_then(Value::as_str).unwrap_or("") != manifest_sha {
        fail(format!(
            "{}: manifest_sha256 does not match bytecode.manifest.json",
            seal_path.display()
        ));
    }
    if seal.get("map_sha256").and_then(Value::as_str).unwrap_or("") != map_sha {
        fail(format!(
            "{}: map_sha256 does not match bytecode.map",
            seal_path.display()
        ));
    }
    let license_path = base_dir.join("bytecode.license.json");
    let license_sha = sha256_file_hex(&license_path);
    match &license_sha {
        Some(ls) => {
            if seal.get("license_sha256").and_then(Value::as_str).unwrap_or("") != *ls {
                fail(format!(
                    "{}: license_sha256 does not match bytecode.license.json",
                    seal_path.display()
                ));
            }
        }
        None => {
            if seal.get("license_sha256").is_some() {
                fail(format!(
                    "{}: seal covers a bytecode.license.json that is missing",
                    seal_path.display()
                ));
            }
        }
    }
    let policy_sha = seal.get("policy_sha256").and_then(Value::as_str);
    if let Some(ps) = policy_sha {
        if !ps.is_empty() {
            let actual = sha256_hex(seal_policy_message(&seal).as_bytes());
            if !ct_eq(&ps.to_lowercase(), &actual) {
                fail(format!(
                    "{}: policy_sha256 does not match signed policy fields",
                    seal_path.display()
                ));
            }
        }
    }

    let pub_key = env_nonempty("BYTECODE_VENDOR_PUBKEY")
        .or_else(|| env_nonempty("OPDUMP_VENDOR_PUBKEY_FILE"))
        .unwrap_or_default();
    if !pub_key.is_empty() {
        if !Path::new(&pub_key).is_file() {
            fail(format!("vendor public key does not exist: {}", pub_key));
        }
        let mut message = String::new();
        message.push_str("bytecode-seal-v1\n");
        message.push_str(&format!("manifest:{manifest_sha}\n"));
        message.push_str(&format!("map:{map_sha}\n"));
        message.push_str(&format!(
            "license:{}\n",
            license_sha.as_deref().unwrap_or("-")
        ));
        if let Some(ps) = policy_sha {
            if !ps.is_empty() {
                message.push_str(&format!("policy:{ps}\n"));
            }
        }
        let signature = base64::engine::general_purpose::STANDARD
            .decode(seal.get("signature").and_then(Value::as_str).unwrap_or(""))
            .ok()
            .filter(|s| s.len() == 64)
            .unwrap_or_else(|| {
                fail(format!("{}: malformed Ed25519 signature", seal_path.display()))
            });
        if !openssl_ed25519_verify(&pub_key, message.as_bytes(), &signature) {
            fail(format!(
                "{}: Ed25519 seal signature is NOT valid for {}",
                seal_path.display(),
                pub_key
            ));
        }
        println!("vendor seal: Ed25519 signature verified against {pub_key}");
    } else {
        println!("vendor seal: digests match (set BYTECODE_VENDOR_PUBKEY to verify the Ed25519 signature)");
    }
}

/// Verify an Ed25519 signature by shelling out to the `openssl` CLI, exactly as
/// the PHP tool does (same trust anchor, same success string).
fn openssl_ed25519_verify(pub_key: &str, message: &[u8], signature: &[u8]) -> bool {
    use std::io::Write;
    let dir = std::env::temp_dir();
    let pid = std::process::id();
    let msg_tmp = dir.join(format!("seal-msg-{pid}"));
    let sig_tmp = dir.join(format!("seal-sig-{pid}"));
    let ok = (|| -> std::io::Result<bool> {
        std::fs::File::create(&msg_tmp)?.write_all(message)?;
        std::fs::File::create(&sig_tmp)?.write_all(signature)?;
        let out = std::process::Command::new("openssl")
            .args(["pkeyutl", "-verify", "-pubin", "-inkey", pub_key, "-rawin", "-in"])
            .arg(&msg_tmp)
            .arg("-sigfile")
            .arg(&sig_tmp)
            .output()?;
        let mut combined = out.stdout;
        combined.extend_from_slice(&out.stderr);
        let text = String::from_utf8_lossy(&combined);
        Ok(out.status.success() && text.contains("Signature Verified Successfully"))
    })()
    .unwrap_or(false);
    let _ = std::fs::remove_file(&msg_tmp);
    let _ = std::fs::remove_file(&sig_tmp);
    ok
}
