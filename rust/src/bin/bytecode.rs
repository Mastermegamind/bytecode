//! `bytecode` — a single front-door CLI that multiplexes every bytecode tool,
//! so agents and humans have one entry point with the full surface:
//!
//!     bytecode <tool> [args...]     # any tool in `ops::TOOLS`
//!     bytecode mcp                  # launch the MCP server (agentic control)
//!     bytecode api [addr]           # launch the HTTP API server
//!     bytecode help
//!
//! Each tool execs `php php/bin/bytecode-<tool>` (which self-dispatches to the
//! Rust binary when present) or a standalone Rust binary, inheriting stdio and
//! the exit code — so there is no capability subset: anything a tool can do on
//! the command line is reachable here.

use std::process::{exit, Command};

use bytecode_tools::ops;

fn usage() {
    eprintln!("Usage: bytecode <command> [args...]");
    eprintln!();
    eprintln!("Tools:");
    for (name, desc) in ops::TOOLS {
        eprintln!("  {name:<16} {desc}");
    }
    eprintln!();
    eprintln!("Servers:");
    eprintln!("  {:<16} launch the MCP server (stdio) for agentic control", "mcp");
    eprintln!("  {:<16} launch the HTTP API server (default 127.0.0.1:8787)", "api [addr]");
}

fn sibling(bin: &str) -> String {
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            let p = dir.join(bin);
            if p.is_file() {
                return p.to_string_lossy().into_owned();
            }
        }
    }
    bin.to_string()
}

fn exec(mut cmd: Command) -> ! {
    match cmd.status() {
        Ok(status) => exit(status.code().unwrap_or(1)),
        Err(e) => {
            eprintln!("bytecode: failed to launch: {e}");
            exit(127);
        }
    }
}

fn main() {
    let argv: Vec<String> = std::env::args().collect();
    if argv.len() < 2 || matches!(argv[1].as_str(), "-h" | "--help" | "help") {
        usage();
        exit(if argv.len() < 2 { 2 } else { 0 });
    }

    let sub = argv[1].as_str();
    let rest = &argv[2..];

    // Server launchers.
    if sub == "mcp" || sub == "api" {
        let mut cmd = Command::new(sibling(&format!("bytecode-{sub}")));
        cmd.args(rest);
        exec(cmd);
    }

    // Any registered tool: resolve (PHP launcher is Rust-first) and exec,
    // inheriting stdio; fall back to a standalone Rust binary if PHP is absent.
    if ops::is_tool(sub) {
        match ops::resolve_tool(sub) {
            Ok((prog, pre)) => {
                let mut cmd = Command::new(&prog);
                cmd.args(&pre).args(rest);
                match cmd.status() {
                    Ok(status) => exit(status.code().unwrap_or(1)),
                    Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                        let mut cmd = Command::new(sibling(&format!("bytecode-{sub}")));
                        cmd.args(rest);
                        exec(cmd);
                    }
                    Err(e) => {
                        eprintln!("bytecode: failed to launch {sub}: {e}");
                        exit(127);
                    }
                }
            }
            Err(e) => {
                eprintln!("bytecode: {e}");
                exit(2);
            }
        }
    }

    eprintln!("bytecode: unknown command: {sub}");
    usage();
    exit(2);
}
