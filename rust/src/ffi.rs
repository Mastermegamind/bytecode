//! C ABI over [`crate::ops`] so the Flutter desktop app (or any C/Dart FFI
//! caller) can run keygen / pack / verify **in-process**, without spawning the
//! CLI tools or a PHP runtime.
//!
//! Conventions
//! -----------
//! * Strings in are NUL-terminated UTF-8 `const char*`.
//! * Strings out are heap-allocated by the library; the caller must release
//!   them with [`bt_string_free`]. Never free them with libc `free`.
//! * Functions returning `int` use `0` for success and `-1` for failure; on
//!   failure a human-readable message is available from [`bt_last_error`] until
//!   the next FFI call on the same thread.
//! * `bt_last_error` returns a pointer owned by the library (thread-local);
//!   copy it if you need it past the next call.

use std::cell::RefCell;
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int};
use std::path::Path;

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::default());
}

fn set_last_error(msg: &str) {
    let c = CString::new(msg).unwrap_or_else(|_| CString::new("error").unwrap());
    LAST_ERROR.with(|e| *e.borrow_mut() = c);
}

fn clear_last_error() {
    LAST_ERROR.with(|e| *e.borrow_mut() = CString::default());
}

/// Last error message on this thread (empty string if none). Owned by the
/// library; valid until the next FFI call on this thread.
#[no_mangle]
pub extern "C" fn bt_last_error() -> *const c_char {
    LAST_ERROR.with(|e| e.borrow().as_ptr())
}

/// Free a string returned by this library (e.g. from `bt_keygen_hex` or the
/// `out_json` of `bt_verify_one`). Passing NULL is a no-op.
///
/// # Safety
/// `s` must be a pointer this library returned and not yet freed.
#[no_mangle]
pub unsafe extern "C" fn bt_string_free(s: *mut c_char) {
    if !s.is_null() {
        drop(CString::from_raw(s));
    }
}

unsafe fn cstr<'a>(p: *const c_char, what: &str) -> Result<&'a str, ()> {
    if p.is_null() {
        set_last_error(&format!("{what} is NULL"));
        return Err(());
    }
    CStr::from_ptr(p).to_str().map_err(|_| {
        set_last_error(&format!("{what} is not valid UTF-8"));
    })
}

fn alloc(s: String) -> *mut c_char {
    CString::new(s).map(CString::into_raw).unwrap_or(std::ptr::null_mut())
}

/// Generate a fresh 32-byte key as 64 hex characters. Returns a heap string the
/// caller frees with `bt_string_free`, or NULL on allocation failure.
#[no_mangle]
pub extern "C" fn bt_keygen_hex() -> *mut c_char {
    clear_last_error();
    alloc(crate::ops::keygen_hex())
}

/// Wrap a raw OPD2 blob at `raw_path` into a BYTC v2 container at `out_path`,
/// stamping `php_version_id` into the header. Key material is taken from the
/// usual environment (BYTECODE_DEK/KEY/OPDUMP_KEY/VENDOR_KEY) or the vendor key
/// file, which is created if absent — exactly like `bytecode-pack`.
///
/// Returns 0 on success, -1 on failure (see `bt_last_error`).
///
/// # Safety
/// `raw_path` and `out_path` must be valid NUL-terminated strings.
#[no_mangle]
pub unsafe extern "C" fn bt_pack(
    raw_path: *const c_char,
    out_path: *const c_char,
    php_version_id: u32,
) -> c_int {
    clear_last_error();
    let (raw, out) = match (cstr(raw_path, "raw_path"), cstr(out_path, "out_path")) {
        (Ok(a), Ok(b)) => (a, b),
        _ => return -1,
    };
    match crate::ops::pack(Path::new(raw), Path::new(out), php_version_id) {
        Ok(()) => 0,
        Err(e) => {
            set_last_error(&e);
            -1
        }
    }
}

/// Verify one `.bytc` container. `decrypt_test` != 0 also AES-GCM-decrypts it.
/// `want_php_version_id` is the PHP_VERSION_ID the container must declare.
/// On success writes a JSON summary into `*out_json` (caller frees it with
/// `bt_string_free`) and returns 0; returns -1 on failure (see `bt_last_error`).
///
/// JSON shape:
/// `{"backend":"..","backend_format":"..","sha256":"..","size":N,"payload_size":N|null}`
///
/// # Safety
/// `path` must be a valid NUL-terminated string; `out_json` may be NULL (then
/// no summary is produced) or a valid pointer to a `*mut c_char`.
#[no_mangle]
pub unsafe extern "C" fn bt_verify_one(
    path: *const c_char,
    decrypt_test: c_int,
    want_php_version_id: u32,
    out_json: *mut *mut c_char,
) -> c_int {
    clear_last_error();
    let path = match cstr(path, "path") {
        Ok(p) => p,
        Err(()) => return -1,
    };
    match crate::ops::verify_one(Path::new(path), decrypt_test != 0, want_php_version_id) {
        Ok(r) => {
            if !out_json.is_null() {
                let payload = match r.payload_size {
                    Some(n) => n.to_string(),
                    None => "null".to_string(),
                };
                // backend/format are fixed ASCII tokens and sha256 is hex, so
                // serde_json round-tripping is unnecessary here.
                let json = format!(
                    "{{\"backend\":{},\"backend_format\":{},\"sha256\":\"{}\",\"size\":{},\"payload_size\":{}}}",
                    json_str(&r.backend),
                    json_str(&r.backend_format),
                    r.sha256,
                    r.size,
                    payload
                );
                *out_json = alloc(json);
            }
            0
        }
        Err(e) => {
            set_last_error(&e);
            -1
        }
    }
}

/// Best-effort detection of the local PHP_VERSION_ID: reads
/// `BYTECODE_PHP_VERSION_ID`, else probes a `php` on PATH. Returns the id, or
/// -1 if it cannot be determined (never exits the process).
#[no_mangle]
pub extern "C" fn bt_detect_php_version_id() -> i64 {
    if let Some(v) = crate::env_nonempty("BYTECODE_PHP_VERSION_ID") {
        if let Ok(n) = v.trim().parse::<i64>() {
            return n;
        }
    }
    if let Ok(out) = std::process::Command::new("php")
        .args(["-r", "echo PHP_VERSION_ID;"])
        .output()
    {
        if out.status.success() {
            if let Ok(s) = String::from_utf8(out.stdout) {
                if let Ok(n) = s.trim().parse::<i64>() {
                    return n;
                }
            }
        }
    }
    -1
}

/// Minimal JSON string encoder for the small, known tokens above.
fn json_str(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for ch in s.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}
