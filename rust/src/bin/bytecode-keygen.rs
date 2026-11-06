//! Rust port of `php/bin/bytecode-keygen` — byte-compatible with the PHP tool.
//!
//! Prints a 32-byte key as 64 hex characters, or writes one to a key file
//! (`--out <path>` / `--vendor-secret`), honouring `--force`.

use std::collections::VecDeque;
use std::path::{Path, PathBuf};
use std::process::exit;

use bytecode_tools::{fail, random_bytes, repo_root};

fn usage() {
    eprintln!(
        "Usage:\n  \
bytecode-keygen\n  \
bytecode-keygen --out <path> [--force]\n  \
bytecode-keygen --vendor-secret [--force]\n\n\
Prints a 32-byte key as 64 hex characters, or writes one to a key file.\n\
--vendor-secret writes build/vendor-secret.key under the repository root.\n"
    );
}

#[cfg(unix)]
fn chmod_600(path: &Path) {
    use std::os::unix::fs::PermissionsExt;
    let _ = std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600));
}

#[cfg(not(unix))]
fn chmod_600(_path: &Path) {}

fn main() {
    let argv: Vec<String> = std::env::args().collect();
    let mut args: VecDeque<String> = argv[1..].iter().cloned().collect();

    if matches!(args.front().map(String::as_str), Some("-h") | Some("--help")) {
        usage();
        exit(0);
    }

    let mut out: Option<String> = None;
    let mut force = false;
    while let Some(arg) = args.pop_front() {
        match arg.as_str() {
            "--force" => force = true,
            "--vendor-secret" => {
                out = Some(
                    repo_root()
                        .join("build")
                        .join("vendor-secret.key")
                        .to_string_lossy()
                        .into_owned(),
                );
            }
            "--out" => match args.pop_front() {
                Some(p) => out = Some(p),
                None => fail_code("--out requires a path", 2),
            },
            s if s.starts_with("--out=") => {
                out = Some(s["--out=".len()..].to_string());
            }
            _ => {
                usage();
                exit(2);
            }
        }
    }

    let key = hex::encode(random_bytes(32));

    match out {
        None => {
            println!("{key}");
            exit(0);
        }
        Some(ref o) if o.is_empty() => {
            println!("{key}");
            exit(0);
        }
        Some(o) => {
            let out_path = PathBuf::from(&o);
            if out_path.is_file() && !force {
                fail_code(
                    &format!("key file already exists: {o} (pass --force to regenerate)"),
                    2,
                );
            }
            if let Some(dir) = out_path.parent() {
                if !dir.as_os_str().is_empty() && !dir.is_dir() && std::fs::create_dir_all(dir).is_err() && !dir.is_dir() {
                    fail(format!("cannot create key directory: {}", dir.display()));
                }
            }
            if std::fs::write(&out_path, format!("{key}\n")).is_err() {
                fail(format!("cannot write key file: {o}"));
            }
            chmod_600(&out_path);
            println!("{o}");
        }
    }
}

fn fail_code(msg: &str, code: i32) -> ! {
    eprintln!("{msg}");
    exit(code);
}
