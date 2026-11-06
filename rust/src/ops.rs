//! Operation-level API shared by every front-end (the tool binaries, the C ABI
//! for Flutter, and the CLI/API/MCP adapters). Each function performs one
//! operation and returns a `Result` with the exact human-readable message the
//! PHP/CLI tools emit on failure — callers decide how to render or exit.

use std::collections::HashMap;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::{Mutex, OnceLock};

use crate::*;

// ---------------------------------------------------------------------------
// Tool registry — the single source of truth for every bytecode-* tool, shared
// by the CLI, MCP and API adapters so all three expose the full surface with no
// per-adapter limitation.
// ---------------------------------------------------------------------------

/// Every tool, by its subcommand name (the `bytecode-<name>` suffix). Each has a
/// `php/bin/bytecode-<name>` script that self-dispatches to the Rust binary when
/// one is present, so running the script is Rust-first and complete.
pub const TOOLS: &[(&str, &str)] = &[
    ("deploy", "Deploy an encoded build to a remote target."),
    ("doctor", "Diagnose the local environment and loader/extension setup."),
    ("dump", "Encode PHP sources to BYTC containers (the main encoder; needs the opdump extension)."),
    ("env-pack", "Package environment/key material for a build."),
    ("info", "Print build/loader/container information."),
    ("install-loader", "Install the loader extension for a PHP version."),
    ("keygen", "Generate a 32-byte key (64 hex), or write one to a key file."),
    ("key-rotate", "Rotate key material."),
    ("laravel", "Laravel integration helper."),
    ("license-keygen", "Generate the RSA-4096 license-mode keypair."),
    ("obfuscate", "Apply source-level obfuscation before encoding."),
    ("pack", "Wrap a raw OPD2 blob into a BYTC v2 container."),
    ("pack-asset", "Wrap a non-PHP asset into a BYTC container."),
    ("package-sign", "Write bytecode.package.json for an output directory."),
    ("scan", "Preflight-scan PHP sources for dynamic constructs."),
    ("selftest", "Run the tool self-test."),
    ("vendor-keygen", "Generate the Ed25519 vendor sealing keypair."),
    ("verify", "Verify BYTC containers / manifests; optionally decrypt-test."),
    ("version", "Print the tool version."),
];

pub fn is_tool(name: &str) -> bool {
    TOOLS.iter().any(|(n, _)| *n == name)
}

fn php_bin() -> String {
    env_nonempty("BYTECODE_PHP").unwrap_or_else(|| "php".to_string())
}

fn script_path(tool: &str) -> PathBuf {
    repo_root().join("php").join("bin").join(format!("bytecode-{tool}"))
}

fn sibling_binary(tool: &str) -> Option<PathBuf> {
    let exe = std::env::current_exe().ok()?;
    let dir = exe.parent()?;
    let p = dir.join(format!("bytecode-{tool}"));
    if p.is_file() {
        Some(p)
    } else {
        None
    }
}

/// Resolve how to launch a tool: `(program, leading_args)`. Prefers the PHP
/// script (which is Rust-first via its dispatch shim and covers every tool);
/// falls back to a sibling Rust binary when the script is absent.
pub fn resolve_tool(tool: &str) -> Result<(String, Vec<String>), String> {
    if !is_tool(tool) {
        return Err(format!("unknown tool: {tool}"));
    }
    let script = script_path(tool);
    if script.is_file() {
        return Ok((php_bin(), vec![script.to_string_lossy().into_owned()]));
    }
    if let Some(bin) = sibling_binary(tool) {
        return Ok((bin.to_string_lossy().into_owned(), vec![]));
    }
    Err(format!("tool not found: bytecode-{tool}"))
}

/// Captured result of running a tool (used by MCP/API; the CLI inherits stdio).
pub struct ToolOutput {
    pub code: i32,
    pub stdout: String,
    pub stderr: String,
}

/// Environment keys an adapter (MCP/API) is permitted to forward to a tool.
/// The bytecode tools only read `BYTECODE_*` / `OPDUMP_*` and `PHP_BIN`; anything
/// else is rejected so a remote caller cannot smuggle in `PATH`, `LD_PRELOAD`,
/// etc. NOTE: values forwarded here become process environment and are readable
/// via `/proc/<pid>/environ` by the same OS user — pass secrets accordingly.
pub fn is_allowed_env(key: &str) -> bool {
    key.starts_with("BYTECODE_") || key.starts_with("OPDUMP_") || key == "PHP_BIN"
}

/// Cached `<tool> --help` text, used to enrich MCP/API tool descriptions.
pub fn tool_help(tool: &str) -> String {
    static CACHE: OnceLock<Mutex<HashMap<String, String>>> = OnceLock::new();
    let cache = CACHE.get_or_init(|| Mutex::new(HashMap::new()));
    if let Some(h) = cache.lock().unwrap().get(tool) {
        return h.clone();
    }
    let help = match run_tool(tool, &["--help".to_string()], None, &[]) {
        Ok(o) => {
            let t = if o.stdout.trim().is_empty() { o.stderr } else { o.stdout };
            truncate_chars(t.trim(), 1500)
        }
        Err(_) => String::new(),
    };
    cache.lock().unwrap().insert(tool.to_string(), help.clone());
    help
}

fn truncate_chars(s: &str, max: usize) -> String {
    if s.len() <= max {
        return s.to_string();
    }
    let mut end = max;
    while end > 0 && !s.is_char_boundary(end) {
        end -= 1;
    }
    format!("{}…", &s[..end])
}

/// Run a tool to completion, capturing stdout/stderr. `stdin` is fed to the
/// process; `envs` are overlaid on the current environment. Launch falls back
/// to a sibling Rust binary if the PHP launcher is missing.
pub fn run_tool(
    tool: &str,
    args: &[String],
    stdin: Option<&[u8]>,
    envs: &[(String, String)],
) -> Result<ToolOutput, String> {
    for (k, _) in envs {
        if !is_allowed_env(k) {
            return Err(format!("environment variable not permitted: {k}"));
        }
    }
    let (prog, pre) = resolve_tool(tool)?;
    let spawn = |prog: &str, pre: &[String]| -> std::io::Result<std::process::Child> {
        let mut cmd = Command::new(prog);
        cmd.args(pre)
            .args(args)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        for (k, v) in envs {
            cmd.env(k, v);
        }
        cmd.spawn()
    };

    let mut child = match spawn(&prog, &pre) {
        Ok(c) => c,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            // PHP launcher missing — try a standalone Rust binary.
            match sibling_binary(tool) {
                Some(bin) => spawn(&bin.to_string_lossy(), &[])
                    .map_err(|e| format!("failed to launch bytecode-{tool}: {e}"))?,
                None => return Err(format!("failed to launch bytecode-{tool}: {e}")),
            }
        }
        Err(e) => return Err(format!("failed to launch bytecode-{tool}: {e}")),
    };

    if let Some(data) = stdin {
        if let Some(mut si) = child.stdin.take() {
            let _ = si.write_all(data);
        }
    } else {
        drop(child.stdin.take());
    }
    let out = child
        .wait_with_output()
        .map_err(|e| format!("failed to run bytecode-{tool}: {e}"))?;
    Ok(ToolOutput {
        code: out.status.code().unwrap_or(-1),
        stdout: String::from_utf8_lossy(&out.stdout).into_owned(),
        stderr: String::from_utf8_lossy(&out.stderr).into_owned(),
    })
}

/// Generate a fresh 32-byte key as 64 hex characters (bytecode-keygen).
pub fn keygen_hex() -> String {
    hex::encode(random_bytes(32))
}

// ---------------------------------------------------------------------------
// pack
// ---------------------------------------------------------------------------

/// Resolve the hex IKM for packing, creating the default vendor key file when
/// nothing is configured (mirrors bytecode-pack). Returns the 64-hex IKM.
fn resolve_or_create_ikm() -> Result<String, String> {
    let have_env = env_nonempty("BYTECODE_DEK").is_some()
        || env_nonempty("BYTECODE_KEY").is_some()
        || env_nonempty("OPDUMP_KEY").is_some()
        || env_nonempty("BYTECODE_VENDOR_KEY").is_some();
    let vkf = vendor_key_file();
    if !vkf.is_file() && !have_env {
        if let Some(dir) = vkf.parent() {
            if !dir.is_dir() && std::fs::create_dir_all(dir).is_err() && !dir.is_dir() {
                return Err(format!("cannot create vendor key directory: {}", dir.display()));
            }
            if dir.is_dir() {
                chmod(dir, 0o770);
            }
        }
        if std::fs::write(&vkf, format!("{}\n", hex::encode(random_bytes(32)))).is_err() {
            return Err(format!("cannot write vendor key file: {}", vkf.display()));
        }
        chmod(&vkf, 0o600);
    }
    Ok(resolve_ikm_hex_decrypt())
}

/// Wrap a raw OPD2 blob into a BYTC v2 container (bytecode-pack).
pub fn pack(raw_path: &Path, out_path: &Path, php_version_id: u32) -> Result<(), String> {
    let hex_key = resolve_or_create_ikm()?;
    if !is_hex64(&hex_key) {
        return Err("BYTECODE_DEK/BYTECODE_KEY/OPDUMP_KEY/BYTECODE_VENDOR_KEY must be exactly 64 hex characters".to_string());
    }
    let raw = std::fs::read(raw_path)
        .map_err(|_| format!("cannot read raw blob: {}", raw_path.display()))?;
    if raw.len() < 4 || &raw[0..4] != b"OPD2" {
        return Err(format!("raw blob is not OPD2: {}", raw_path.display()));
    }

    let ikm = hex::decode(&hex_key).unwrap();
    let key_id = random_bytes(16);
    let key = hkdf_sha256(&ikm, 32, b"bytecode-v2", &key_id);
    let nonce = random_bytes(12);

    let mut header = Vec::new();
    header.extend_from_slice(b"BYTC");
    put_u32le(&mut header, 2);
    put_u32le(&mut header, php_version_id);
    put_lp32(&mut header, BACKEND_PHP.as_bytes());
    put_lp32(&mut header, FORMAT_OPD2.as_bytes());
    put_u32le(&mut header, 1);
    put_lp32(&mut header, &key_id);

    let (ciphertext, tag) = aes256gcm_encrypt(&key, &nonce, &header, &raw)
        .ok_or_else(|| "AES-256-GCM encryption failed".to_string())?;

    let mut container = header;
    put_lp32(&mut container, &nonce);
    put_lp32(&mut container, &tag);
    put_u64le(&mut container, ciphertext.len() as u64);
    container.extend_from_slice(&ciphertext);

    std::fs::write(out_path, &container)
        .map_err(|_| format!("cannot write container: {}", out_path.display()))
}

// ---------------------------------------------------------------------------
// verify (single container)
// ---------------------------------------------------------------------------

/// Structural/metadata result for one container (and, if requested, the
/// decrypted payload length).
pub struct VerifyOne {
    pub backend: String,
    pub backend_format: String,
    pub sha256: String,
    pub size: u64,
    pub payload_size: Option<usize>,
}

fn decrypt_test(path: &Path, c: &Container) -> Result<usize, String> {
    if c.version != 2 {
        return Err(format!("{}: decrypt-test requires BYTC2", path.display()));
    }
    let ikm_hex = resolve_ikm_hex_decrypt();
    if !is_hex64(&ikm_hex) {
        return Err("decrypt-test requires BYTECODE_KEY/OPDUMP_KEY/BYTECODE_VENDOR_KEY/BYTECODE_DEK or build/vendor-secret.key".to_string());
    }
    let ikm = hex::decode(&ikm_hex).unwrap();
    let key = hkdf_sha256(&ikm, 32, b"bytecode-v2", &c.key_id);
    let aad = &c.bytes[0..c.header_size];
    let plain = aes256gcm_decrypt(&key, &c.nonce, &c.tag, aad, &c.ciphertext)
        .ok_or_else(|| format!("{}: decrypt-test failed (wrong key or tampered container)", path.display()))?;
    if c.backend_id == BACKEND_PHP && (plain.len() < 4 || &plain[0..4] != b"OPD2") {
        return Err(format!("{}: decrypt-test payload is not OPD2", path.display()));
    }
    Ok(plain.len())
}

/// Verify one `.bytc` container's header/metadata, optionally decrypting it.
/// `want_php_version_id` is the PHP_VERSION_ID the container must declare.
pub fn verify_one(
    path: &Path,
    decrypt: bool,
    want_php_version_id: u32,
) -> Result<VerifyOne, String> {
    let c = try_parse_bytc(path)?;
    if c.php_version_id != want_php_version_id {
        return Err(format!(
            "{}: PHP_VERSION_ID {} does not match {}",
            path.display(),
            c.php_version_id,
            want_php_version_id
        ));
    }
    let is_php = c.backend_id == BACKEND_PHP && c.backend_format == FORMAT_OPD2;
    let is_asset = c.backend_id == BACKEND_ASSET && c.backend_format == FORMAT_RAW1;
    if !is_php && !is_asset {
        return Err(format!(
            "{}: unsupported backend {}/{}",
            path.display(),
            c.backend_id,
            c.backend_format
        ));
    }
    if c.version == 2 && c.cipher_id != Some(1) {
        return Err(format!(
            "{}: unsupported cipher_id {}",
            path.display(),
            c.cipher_id.unwrap_or(0)
        ));
    }
    if c.nonce.len() != 12 || c.tag.len() != 16 {
        return Err(format!("{}: unsupported nonce/tag length", path.display()));
    }
    let payload_size = if decrypt { Some(decrypt_test(path, &c)?) } else { None };
    Ok(VerifyOne {
        backend: c.backend_id.clone(),
        backend_format: c.backend_format.clone(),
        sha256: sha256_hex(&c.bytes),
        size: c.bytes.len() as u64,
        payload_size,
    })
}

// ---------------------------------------------------------------------------
// High-level encode pipeline (scan? -> dump -> verify?)
// ---------------------------------------------------------------------------

/// Run the encode pipeline in one call, aggregating each stage's output.
///
/// 1. If `scan_paths` is non-empty, run `scan` first; a non-zero scan aborts.
/// 2. Run `dump` with `dump_args` (the encoder; produces the output tree).
/// 3. If `verify_dir` is set, run `verify` on it (optionally `--decrypt-test`).
///
/// `verify_dir` is explicit (not parsed from `dump_args`) so the pipeline makes
/// no assumptions about the encoder's flags. The returned `code` is the first
/// non-zero stage code (0 if all stages pass).
pub fn encode(
    dump_args: &[String],
    scan_paths: &[String],
    verify_dir: Option<&str>,
    decrypt_test: bool,
    envs: &[(String, String)],
) -> Result<ToolOutput, String> {
    let mut out = String::new();
    let mut err = String::new();

    let append = |label: &str, o: &ToolOutput, out: &mut String, err: &mut String| {
        out.push_str(&format!("=== {label} (exit {}) ===\n", o.code));
        out.push_str(&o.stdout);
        if !o.stdout.ends_with('\n') && !o.stdout.is_empty() {
            out.push('\n');
        }
        if !o.stderr.trim().is_empty() {
            err.push_str(&format!("=== {label} ===\n"));
            err.push_str(&o.stderr);
        }
    };

    if !scan_paths.is_empty() {
        let o = run_tool("scan", scan_paths, None, envs)?;
        append("scan", &o, &mut out, &mut err);
        if o.code != 0 {
            return Ok(ToolOutput { code: o.code, stdout: out, stderr: err });
        }
    }

    let o = run_tool("dump", dump_args, None, envs)?;
    append("dump", &o, &mut out, &mut err);
    if o.code != 0 {
        return Ok(ToolOutput { code: o.code, stdout: out, stderr: err });
    }

    let mut code = 0;
    if let Some(dir) = verify_dir {
        let mut vargs = Vec::new();
        if decrypt_test {
            vargs.push("--decrypt-test".to_string());
        }
        vargs.push(dir.to_string());
        let o = run_tool("verify", &vargs, None, envs)?;
        append("verify", &o, &mut out, &mut err);
        code = o.code;
    }

    Ok(ToolOutput { code, stdout: out, stderr: err })
}
