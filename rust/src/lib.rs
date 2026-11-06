//! Shared building blocks for the Rust ports of the Bytecode `php/bin` tools.
//!
//! Everything here is a faithful, byte-compatible reimplementation of the PHP
//! originals so a Rust build and the PHP fallback produce identical results and
//! accept/reject the exact same containers. See `php/bin/bytecode-verify` and
//! `php/bin/bytecode-pack` for the reference implementations.

use std::path::{Path, PathBuf};
use std::process::exit;

pub mod ffi;
pub mod ops;

use aes_gcm::aead::{Aead, KeyInit, Payload};
use aes_gcm::{Aes256Gcm, Key, Nonce};
use hkdf::Hkdf;
use hmac::{Hmac, Mac};
use sha2::{Digest, Sha256};

/// Print `msg` to stderr and exit 1 — mirrors the PHP `fail()` helper.
pub fn fail(msg: impl AsRef<str>) -> ! {
    eprintln!("{}", msg.as_ref());
    exit(1);
}

/// Restore the default SIGPIPE disposition for CLI tools. Rust sets SIGPIPE to
/// SIG_IGN at startup, which turns a closed stdout (e.g. `bytecode --help |
/// head`) into an EPIPE that `println!`/`eprintln!` then panics on (exit 101).
/// Resetting to SIG_DFL makes the process terminate quietly instead.
///
/// NOTE: do not call this in a long-lived socket server (e.g. the HTTP API):
/// there SIG_IGN is what lets a client disconnect surface as an `io::Error`
/// rather than killing the whole process.
pub fn reset_sigpipe() {
    #[cfg(unix)]
    unsafe {
        extern "C" {
            fn signal(signum: i32, handler: usize) -> usize;
        }
        const SIGPIPE: i32 = 13;
        const SIG_DFL: usize = 0;
        signal(SIGPIPE, SIG_DFL);
    }
}

// ---------------------------------------------------------------------------
// Little-endian reader (mirrors u32le / u64le / take in bytecode-verify)
// ---------------------------------------------------------------------------

pub struct Reader<'a> {
    pub buf: &'a [u8],
    pub off: usize,
}

impl<'a> Reader<'a> {
    pub fn new(buf: &'a [u8]) -> Self {
        Self { buf, off: 0 }
    }

    pub fn u32le(&mut self) -> u32 {
        if self.buf.len() < self.off + 4 {
            fail("truncated container");
        }
        let v = u32::from_le_bytes(self.buf[self.off..self.off + 4].try_into().unwrap());
        self.off += 4;
        v
    }

    pub fn u64le(&mut self) -> u64 {
        if self.buf.len() < self.off + 8 {
            fail("truncated container");
        }
        let v = u64::from_le_bytes(self.buf[self.off..self.off + 8].try_into().unwrap());
        self.off += 8;
        v
    }

    pub fn take(&mut self, len: usize) -> &'a [u8] {
        // PHP guards `$length < 0` — unsigned here, but an enormous u64 length
        // cast to usize is still caught by the bounds check below.
        if self.buf.len() < self.off.saturating_add(len) {
            fail("truncated container");
        }
        let s = &self.buf[self.off..self.off + len];
        self.off += len;
        s
    }
}

// ---------------------------------------------------------------------------
// BYTC container
// ---------------------------------------------------------------------------

pub const BACKEND_PHP: &str = "php-zend-opdump";
pub const FORMAT_OPD2: &str = "OPD2";
pub const BACKEND_ASSET: &str = "bytecode-asset";
pub const FORMAT_RAW1: &str = "RAW1";

pub struct Container {
    pub bytes: Vec<u8>,
    pub version: u32,
    pub php_version_id: u32,
    pub backend_id: String,
    pub backend_format: String,
    pub cipher_id: Option<u32>,
    pub key_id: Vec<u8>,
    pub header_size: usize,
    pub nonce: Vec<u8>,
    pub tag: Vec<u8>,
    pub ciphertext: Vec<u8>,
}

/// Parse a `.bytc` container exactly as `parse_bytc()` does in the PHP tool.
/// Exits the process with the matching message on malformed input (used by the
/// tool binaries). Library callers should use [`try_parse_bytc`] instead.
pub fn parse_bytc(path: &Path) -> Container {
    match try_parse_bytc(path) {
        Ok(c) => c,
        Err(msg) => fail(msg),
    }
}

/// Result-returning container parser for library/FFI callers — identical checks
/// and messages to [`parse_bytc`], but never exits the process.
pub fn try_parse_bytc(path: &Path) -> Result<Container, String> {
    let bytes = std::fs::read(path).map_err(|_| format!("cannot read {}", path.display()))?;
    if bytes.len() < 4 || &bytes[0..4] != b"BYTC" {
        return Err(format!("not a BYTC container: {}", path.display()));
    }

    let mut off = 4usize;
    let u32le = |off: &mut usize| -> Result<u32, String> {
        if bytes.len() < *off + 4 {
            return Err("truncated container".to_string());
        }
        let v = u32::from_le_bytes(bytes[*off..*off + 4].try_into().unwrap());
        *off += 4;
        Ok(v)
    };
    let u64le = |off: &mut usize| -> Result<u64, String> {
        if bytes.len() < *off + 8 {
            return Err("truncated container".to_string());
        }
        let v = u64::from_le_bytes(bytes[*off..*off + 8].try_into().unwrap());
        *off += 8;
        Ok(v)
    };
    let take = |off: &mut usize, len: usize| -> Result<Vec<u8>, String> {
        if bytes.len() < off.saturating_add(len) {
            return Err("truncated container".to_string());
        }
        let s = bytes[*off..*off + len].to_vec();
        *off += len;
        Ok(s)
    };

    let version = u32le(&mut off)?;
    if version != 1 && version != 2 {
        return Err(format!("{}: unsupported container version {}", path.display(), version));
    }
    let php_version_id = u32le(&mut off)?;
    let n = u32le(&mut off)? as usize;
    let backend_id = String::from_utf8_lossy(&take(&mut off, n)?).into_owned();
    let n = u32le(&mut off)? as usize;
    let backend_format = String::from_utf8_lossy(&take(&mut off, n)?).into_owned();
    let mut cipher_id = None;
    let mut key_id = Vec::new();
    if version == 2 {
        cipher_id = Some(u32le(&mut off)?);
        let n = u32le(&mut off)? as usize;
        key_id = take(&mut off, n)?;
    }
    let header_size = off;
    let n = u32le(&mut off)? as usize;
    let nonce = take(&mut off, n)?;
    let n = u32le(&mut off)? as usize;
    let tag = take(&mut off, n)?;
    let n = u64le(&mut off)? as usize;
    let ciphertext = take(&mut off, n)?;
    if off != bytes.len() {
        return Err(format!("{}: trailing bytes after ciphertext", path.display()));
    }

    Ok(Container {
        bytes,
        version,
        php_version_id,
        backend_id,
        backend_format,
        cipher_id,
        key_id,
        header_size,
        nonce,
        tag,
        ciphertext,
    })
}

// ---------------------------------------------------------------------------
// Environment / key material (mirrors resolve_ikm_hex and the manifest IKM)
// ---------------------------------------------------------------------------

/// `getenv(name) ?: ...` semantics: absent OR empty counts as "not set".
pub fn env_nonempty(name: &str) -> Option<String> {
    match std::env::var(name) {
        Ok(v) if !v.is_empty() => Some(v),
        _ => None,
    }
}

/// The repository root the PHP tools compute as `dirname(__DIR__, 2)`.
/// In dispatch mode the PHP wrapper passes `BYTECODE_ROOT`; otherwise we derive
/// it from the binary's own location (`<root>/rust/target/<profile>/<bin>`).
pub fn repo_root() -> PathBuf {
    if let Some(r) = env_nonempty("BYTECODE_ROOT") {
        return PathBuf::from(r);
    }
    if let Ok(exe) = std::env::current_exe() {
        // bin -> profile -> target -> rust -> root
        if let Some(root) = exe.ancestors().nth(4) {
            return root.to_path_buf();
        }
    }
    PathBuf::from(".")
}

pub fn vendor_key_file() -> PathBuf {
    if let Some(p) = env_nonempty("BYTECODE_VENDOR_KEY_FILE") {
        return PathBuf::from(p);
    }
    repo_root().join("build").join("vendor-secret.key")
}

fn file_key() -> Option<String> {
    let p = vendor_key_file();
    match std::fs::read_to_string(&p) {
        Ok(s) => {
            let t = s.trim().to_string();
            if t.is_empty() {
                None
            } else {
                Some(t)
            }
        }
        Err(_) => None,
    }
}

/// IKM used by `--decrypt-test`: BYTECODE_DEK first, then the shared keys, then
/// the vendor key file.
pub fn resolve_ikm_hex_decrypt() -> String {
    env_nonempty("BYTECODE_DEK")
        .or_else(|| env_nonempty("BYTECODE_KEY"))
        .or_else(|| env_nonempty("OPDUMP_KEY"))
        .or_else(|| env_nonempty("BYTECODE_VENDOR_KEY"))
        .or_else(file_key)
        .unwrap_or_default()
}

/// IKM used for the manifest HMAC signature (note: no BYTECODE_DEK here).
pub fn resolve_ikm_hex_manifest() -> String {
    env_nonempty("BYTECODE_KEY")
        .or_else(|| env_nonempty("OPDUMP_KEY"))
        .or_else(|| env_nonempty("BYTECODE_VENDOR_KEY"))
        .or_else(file_key)
        .unwrap_or_default()
}

pub fn is_hex64(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|b| b.is_ascii_hexdigit())
}

// ---------------------------------------------------------------------------
// Crypto primitives (match PHP hash_hkdf / hash_hmac / openssl_decrypt)
// ---------------------------------------------------------------------------

/// HKDF-SHA256. An empty `salt` reproduces PHP's `hash_hkdf(..., salt='')`
/// which RFC 5869 defines as a HashLen-of-zeros salt (crate `new(None, ..)`).
pub fn hkdf_sha256(ikm: &[u8], len: usize, info: &[u8], salt: &[u8]) -> Vec<u8> {
    let salt_opt = if salt.is_empty() { None } else { Some(salt) };
    let hk = Hkdf::<Sha256>::new(salt_opt, ikm);
    let mut okm = vec![0u8; len];
    hk.expand(info, &mut okm)
        .unwrap_or_else(|_| fail("HKDF expand failed"));
    okm
}

pub fn hmac_sha256_hex(key: &[u8], data: &[u8]) -> String {
    let mut mac =
        <Hmac<Sha256> as Mac>::new_from_slice(key).unwrap_or_else(|_| fail("HMAC key error"));
    mac.update(data);
    hex::encode(mac.finalize().into_bytes())
}

pub fn sha256_hex(data: &[u8]) -> String {
    let mut h = Sha256::new();
    h.update(data);
    hex::encode(h.finalize())
}

pub fn sha256_file_hex(path: &Path) -> Option<String> {
    let data = std::fs::read(path).ok()?;
    Some(sha256_hex(&data))
}

/// AES-256-GCM decrypt with associated data. Returns plaintext or None on an
/// authentication failure — same pass/fail decision as `openssl_decrypt`.
pub fn aes256gcm_decrypt(
    key: &[u8],
    nonce: &[u8],
    tag: &[u8],
    aad: &[u8],
    ciphertext: &[u8],
) -> Option<Vec<u8>> {
    if key.len() != 32 || nonce.len() != 12 {
        return None;
    }
    let cipher = Aes256Gcm::new(Key::<Aes256Gcm>::from_slice(key));
    // aes-gcm expects ciphertext followed by the 16-byte tag.
    let mut ct = Vec::with_capacity(ciphertext.len() + tag.len());
    ct.extend_from_slice(ciphertext);
    ct.extend_from_slice(tag);
    cipher
        .decrypt(Nonce::from_slice(nonce), Payload { msg: &ct, aad })
        .ok()
}

/// AES-256-GCM encrypt with associated data. Returns `(ciphertext, tag)` split
/// apart the way the BYTC container stores them (openssl_encrypt with a 16-byte
/// tag passed out separately).
pub fn aes256gcm_encrypt(
    key: &[u8],
    nonce: &[u8],
    aad: &[u8],
    plaintext: &[u8],
) -> Option<(Vec<u8>, Vec<u8>)> {
    if key.len() != 32 || nonce.len() != 12 {
        return None;
    }
    let cipher = Aes256Gcm::new(Key::<Aes256Gcm>::from_slice(key));
    // aes-gcm returns ciphertext followed by the 16-byte tag.
    let mut out = cipher
        .encrypt(Nonce::from_slice(nonce), Payload { msg: plaintext, aad })
        .ok()?;
    if out.len() < 16 {
        return None;
    }
    let tag = out.split_off(out.len() - 16);
    Some((out, tag))
}

/// Cryptographically secure random bytes from the OS RNG (random_bytes()).
pub fn random_bytes(n: usize) -> Vec<u8> {
    let mut buf = vec![0u8; n];
    getrandom::getrandom(&mut buf).unwrap_or_else(|_| fail("OS RNG unavailable"));
    buf
}

/// Append a little-endian u32 (pack('V', ..)).
pub fn put_u32le(out: &mut Vec<u8>, v: u32) {
    out.extend_from_slice(&v.to_le_bytes());
}

/// Append a little-endian u64 (pack('P', ..)).
pub fn put_u64le(out: &mut Vec<u8>, v: u64) {
    out.extend_from_slice(&v.to_le_bytes());
}

/// Append a length-prefixed (u32 LE) byte field, as the container uses for
/// backend id/format, key id, nonce and tag.
pub fn put_lp32(out: &mut Vec<u8>, bytes: &[u8]) {
    put_u32le(out, bytes.len() as u32);
    out.extend_from_slice(bytes);
}

/// Constant-time comparison of two equal-purpose strings (hash_equals).
pub fn ct_eq(a: &str, b: &str) -> bool {
    let (a, b) = (a.as_bytes(), b.as_bytes());
    if a.len() != b.len() {
        return false;
    }
    let mut diff = 0u8;
    for i in 0..a.len() {
        diff |= a[i] ^ b[i];
    }
    diff == 0
}

/// Run an `openssl` subcommand, optionally feeding `stdin`. Returns
/// `(exit_code, stdout_bytes, stderr_string)`. Payloads handled by the keygen
/// tools (PEM/DER) are tiny, so a single blocking write to the pipe is safe.
pub fn openssl_run(args: &[&str], stdin: Option<&[u8]>) -> (i32, Vec<u8>, String) {
    use std::io::Write;
    use std::process::{Command, Stdio};
    let mut child = match Command::new("openssl")
        .args(args)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
    {
        Ok(c) => c,
        Err(_) => fail(format!("failed to start: openssl {}", args.join(" "))),
    };
    if let Some(data) = stdin {
        let _ = child.stdin.take().unwrap().write_all(data);
    } else {
        drop(child.stdin.take());
    }
    let out = match child.wait_with_output() {
        Ok(o) => o,
        Err(_) => fail(format!("failed to run: openssl {}", args.join(" "))),
    };
    (
        out.status.code().unwrap_or(1),
        out.stdout,
        String::from_utf8_lossy(&out.stderr).into_owned(),
    )
}

/// chmod a file to the given mode (no-op on non-unix).
#[cfg(unix)]
pub fn chmod(path: &Path, mode: u32) {
    use std::os::unix::fs::PermissionsExt;
    let _ = std::fs::set_permissions(path, std::fs::Permissions::from_mode(mode));
}

#[cfg(not(unix))]
pub fn chmod(_path: &Path, _mode: u32) {}

/// The PHP_VERSION_ID the PHP tool would check against. In dispatch mode the
/// wrapper exports BYTECODE_PHP_VERSION_ID; otherwise we ask the `php` binary.
pub fn php_version_id() -> u32 {
    if let Some(v) = env_nonempty("BYTECODE_PHP_VERSION_ID") {
        return v
            .trim()
            .parse()
            .unwrap_or_else(|_| fail("BYTECODE_PHP_VERSION_ID is not an integer"));
    }
    if let Ok(out) = std::process::Command::new("php")
        .args(["-r", "echo PHP_VERSION_ID;"])
        .output()
    {
        if out.status.success() {
            if let Ok(s) = String::from_utf8(out.stdout) {
                if let Ok(id) = s.trim().parse() {
                    return id;
                }
            }
        }
    }
    fail("cannot determine target PHP_VERSION_ID: set BYTECODE_PHP_VERSION_ID or install php");
}
