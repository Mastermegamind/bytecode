//! Rust port of `php/bin/bytecode-package-sign` — byte-compatible with the PHP
//! tool. Walks an encoded output directory, records each file's sha256 and size,
//! and writes `bytecode.package.json` (a `bytecode-package-v1` manifest whose
//! own `sha256` covers the compact JSON encoding of the file list).
//!
//! The JSON is emitted to match PHP's `json_encode(..., JSON_PRETTY_PRINT |
//! JSON_UNESCAPED_SLASHES)`: 4-space indentation, slashes left unescaped, and
//! non-ASCII escaped as `\uXXXX`.

use std::path::{Path, PathBuf};
use std::process::exit;
use std::time::{SystemTime, UNIX_EPOCH};

use bytecode_tools::{repo_root, sha256_file_hex, sha256_hex};

struct Entry {
    path: String,
    sha256: String,
    size: u64,
}

fn fail(msg: impl AsRef<str>, code: i32) -> ! {
    eprintln!("{}", msg.as_ref());
    exit(code);
}

/// Escape a string the way PHP's json_encode does with JSON_UNESCAPED_SLASHES
/// (default flags otherwise): escape `"` and `\`, the short control escapes,
/// other control chars and all non-ASCII as lowercase `\uXXXX`; leave `/` and
/// printable ASCII as-is.
fn json_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for ch in s.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\u{08}' => out.push_str("\\b"),
            '\u{09}' => out.push_str("\\t"),
            '\u{0A}' => out.push_str("\\n"),
            '\u{0C}' => out.push_str("\\f"),
            '\u{0D}' => out.push_str("\\r"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c if (c as u32) < 0x80 => out.push(c),
            c => {
                // Non-ASCII: emit \uXXXX, with a surrogate pair above the BMP.
                let cp = c as u32;
                if cp <= 0xFFFF {
                    out.push_str(&format!("\\u{:04x}", cp));
                } else {
                    let v = cp - 0x10000;
                    let hi = 0xD800 + (v >> 10);
                    let lo = 0xDC00 + (v & 0x3FF);
                    out.push_str(&format!("\\u{:04x}\\u{:04x}", hi, lo));
                }
            }
        }
    }
    out.push('"');
    out
}

/// Compact encoding of the files array — the exact bytes PHP hashes for the
/// package `sha256` (`json_encode($files, JSON_UNESCAPED_SLASHES)`).
fn files_compact_json(files: &[Entry]) -> String {
    let mut s = String::from("[");
    for (i, f) in files.iter().enumerate() {
        if i > 0 {
            s.push(',');
        }
        s.push_str("{\"path\":");
        s.push_str(&json_escape(&f.path));
        s.push_str(",\"sha256\":");
        s.push_str(&json_escape(&f.sha256));
        s.push_str(",\"size\":");
        s.push_str(&f.size.to_string());
        s.push('}');
    }
    s.push(']');
    s
}

/// Pretty package manifest, matching PHP's JSON_PRETTY_PRINT (4-space indent).
fn package_pretty_json(
    tool_version: &str,
    created_at: &str,
    files: &[Entry],
    files_sha256: &str,
) -> String {
    let mut s = String::new();
    s.push_str("{\n");
    s.push_str("    \"format\": \"bytecode-package-v1\",\n");
    s.push_str(&format!("    \"tool_version\": {},\n", json_escape(tool_version)));
    s.push_str(&format!("    \"created_at\": {},\n", json_escape(created_at)));
    s.push_str(&format!("    \"file_count\": {},\n", files.len()));
    if files.is_empty() {
        s.push_str("    \"files\": [],\n");
    } else {
        s.push_str("    \"files\": [\n");
        for (i, f) in files.iter().enumerate() {
            s.push_str("        {\n");
            s.push_str(&format!("            \"path\": {},\n", json_escape(&f.path)));
            s.push_str(&format!("            \"sha256\": {},\n", json_escape(&f.sha256)));
            s.push_str(&format!("            \"size\": {}\n", f.size));
            s.push_str("        }");
            s.push_str(if i + 1 < files.len() { ",\n" } else { "\n" });
        }
        s.push_str("    ],\n");
    }
    s.push_str(&format!("    \"sha256\": {}\n", json_escape(files_sha256)));
    s.push('}');
    s
}

/// gmdate('c') — ISO-8601 in UTC, e.g. 2026-11-06T01:23:45+00:00.
fn gmdate_c() -> String {
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0);
    let days = secs.div_euclid(86400);
    let rem = secs.rem_euclid(86400);
    let (h, mi, s) = (rem / 3600, (rem % 3600) / 60, rem % 60);
    let (y, m, d) = civil_from_days(days);
    format!("{y:04}-{m:02}-{d:02}T{h:02}:{mi:02}:{s:02}+00:00")
}

/// Howard Hinnant's civil-from-days algorithm (days since 1970-01-01).
fn civil_from_days(z: i64) -> (i64, i64, i64) {
    let z = z + 719468;
    let era = if z >= 0 { z } else { z - 146096 } / 146097;
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    (y + if m <= 2 { 1 } else { 0 }, m, d)
}

fn collect(dir: &Path, base: &Path, out: &mut Vec<Entry>) {
    let rd = match std::fs::read_dir(dir) {
        Ok(rd) => rd,
        Err(_) => return,
    };
    for entry in rd.flatten() {
        let path = entry.path();
        let ft = match entry.file_type() {
            Ok(ft) => ft,
            Err(_) => continue,
        };
        if ft.is_dir() {
            collect(&path, base, out);
        } else if ft.is_file() {
            if path.file_name().map(|n| n == "bytecode.package.json").unwrap_or(false) {
                continue;
            }
            let rel = path
                .strip_prefix(base)
                .unwrap_or(&path)
                .to_string_lossy()
                .replace('\\', "/");
            let sha = sha256_file_hex(&path).unwrap_or_else(|| {
                fail(format!("cannot read {}", path.display()), 1)
            });
            let size = std::fs::metadata(&path).map(|m| m.len()).unwrap_or(0);
            out.push(Entry { path: rel, sha256: sha, size });
        }
    }
}

fn project_version() -> String {
    let p = repo_root().join("VERSION");
    match std::fs::read_to_string(&p) {
        Ok(s) => {
            let t = s.trim().to_string();
            if t.is_empty() {
                "v0.0.0-dev".to_string()
            } else {
                t
            }
        }
        Err(_) => "v0.0.0-dev".to_string(),
    }
}

fn main() {
    let argv: Vec<String> = std::env::args().collect();
    let args = &argv[1..];
    let is_help = args.len() == 1 && matches!(args[0].as_str(), "-h" | "--help");
    if args.is_empty() || is_help {
        eprintln!("Usage: bytecode-package-sign <encoded-output-dir>");
        exit(if is_help { 0 } else { 2 });
    }

    let dir = args[0].trim_end_matches('/').to_string();
    let dir_path = PathBuf::from(&dir);
    if !dir_path.is_dir() {
        fail(format!("not a directory: {dir}"), 2);
    }

    let mut files: Vec<Entry> = Vec::new();
    collect(&dir_path, &dir_path, &mut files);
    // usort by 'path' (PHP spaceship on the path string == byte order on UTF-8).
    files.sort_by(|a, b| a.path.cmp(&b.path));

    let files_sha = sha256_hex(files_compact_json(&files).as_bytes());
    let json = package_pretty_json(&project_version(), &gmdate_c(), &files, &files_sha);

    let out_path = dir_path.join("bytecode.package.json");
    if std::fs::write(&out_path, format!("{json}\n")).is_err() {
        fail(format!("cannot write {}", out_path.display()), 1);
    }
    println!("wrote {dir}/bytecode.package.json");
}
