//! `bytecode-mcp` — a Model Context Protocol server over stdio (newline-
//! delimited JSON-RPC 2.0) exposing **every** bytecode tool, so an MCP-speaking
//! agent can drive the full pipeline: encode (dump), obfuscate, pack, verify,
//! sign/seal, keygen, deploy, doctor, scan, and more — no capability subset.
//!
//! Each tool in `ops::TOOLS` becomes an MCP tool whose input is
//!   { "args": [string...], "stdin"?: string, "env"?: { KEY: VALUE } }
//! giving the agent the tool's complete command-line surface. Handlers run the
//! canonical launcher (Rust-first via the PHP dispatch shim) and return
//! stdout/stderr/exit_code.

use std::io::{BufRead, Write};
use std::path::PathBuf;

use bytecode_tools::{ops, repo_root};
use serde_json::{json, Value};

const PROTOCOL_VERSION: &str = "2024-11-05";

fn main() {
    let stdin = std::io::stdin();
    let stdout = std::io::stdout();
    let mut out = stdout.lock();

    for line in stdin.lock().lines() {
        let line = match line {
            Ok(l) => l,
            Err(_) => break,
        };
        if line.trim().is_empty() {
            continue;
        }
        let req: Value = match serde_json::from_str(&line) {
            Ok(v) => v,
            Err(e) => {
                let _ = writeln!(out, "{}", error_response(&Value::Null, -32700, &format!("parse error: {e}")));
                let _ = out.flush();
                continue;
            }
        };
        let id = req.get("id").cloned();
        let method = req.get("method").and_then(Value::as_str).unwrap_or("");
        let params = req.get("params").cloned().unwrap_or(Value::Null);

        // Notifications (no id) get no response.
        let Some(id) = id else { continue };

        let response = match method {
            "initialize" => success(&id, initialize_result()),
            "tools/list" => success(&id, json!({ "tools": tool_specs() })),
            "ping" => success(&id, json!({})),
            "tools/call" => match call_tool(&params) {
                Ok(result) => success(&id, result),
                Err(msg) => success(&id, tool_error(&msg)),
            },
            other => error_response(&id, -32601, &format!("method not found: {other}")),
        };
        let _ = writeln!(out, "{response}");
        let _ = out.flush();
    }
}

fn initialize_result() -> Value {
    json!({
        "protocolVersion": PROTOCOL_VERSION,
        "capabilities": { "tools": {} },
        "serverInfo": { "name": "bytecode-tools", "version": project_version() }
    })
}

fn success(id: &Value, result: Value) -> String {
    json!({ "jsonrpc": "2.0", "id": id, "result": result }).to_string()
}

fn error_response(id: &Value, code: i64, message: &str) -> String {
    json!({ "jsonrpc": "2.0", "id": id, "error": { "code": code, "message": message } }).to_string()
}

fn text_result(text: String, is_error: bool) -> Value {
    json!({ "content": [ { "type": "text", "text": text } ], "isError": is_error })
}

fn tool_error(msg: &str) -> Value {
    text_result(msg.to_string(), true)
}

/// One MCP tool per registered bytecode tool, each with the full CLI surface
/// via a generic `args` array (plus optional stdin and env overlay).
fn tool_specs() -> Value {
    let input_schema = json!({
        "type": "object",
        "properties": {
            "args": {
                "type": "array",
                "items": { "type": "string" },
                "description": "command-line arguments passed verbatim to the tool"
            },
            "stdin": { "type": "string", "description": "optional data written to the tool's stdin" },
            "env": {
                "type": "object",
                "additionalProperties": { "type": "string" },
                "description": "environment overrides (e.g. BYTECODE_KEY, BYTECODE_VENDOR_PUBKEY)"
            }
        },
        "required": []
    });
    let tools: Vec<Value> = ops::TOOLS
        .iter()
        .map(|(name, desc)| {
            json!({
                "name": *name,
                "description": format!("{desc} Pass CLI args in `args` (e.g. run with [\"--help\"] to see usage)."),
                "inputSchema": input_schema
            })
        })
        .collect();
    Value::Array(tools)
}

fn call_tool(params: &Value) -> Result<Value, String> {
    let name = params
        .get("name")
        .and_then(Value::as_str)
        .ok_or_else(|| "tools/call missing 'name'".to_string())?;
    if !ops::is_tool(name) {
        return Err(format!("unknown tool: {name}"));
    }
    let arguments = params.get("arguments").cloned().unwrap_or(json!({}));

    let args: Vec<String> = arguments
        .get("args")
        .and_then(Value::as_array)
        .map(|a| a.iter().filter_map(|v| v.as_str().map(str::to_string)).collect())
        .unwrap_or_default();

    let stdin = arguments.get("stdin").and_then(Value::as_str).map(|s| s.as_bytes().to_vec());

    let envs: Vec<(String, String)> = arguments
        .get("env")
        .and_then(Value::as_object)
        .map(|m| {
            m.iter()
                .filter_map(|(k, v)| v.as_str().map(|s| (k.clone(), s.to_string())))
                .collect()
        })
        .unwrap_or_default();

    let output = ops::run_tool(name, &args, stdin.as_deref(), &envs)?;

    let mut text = format!("exit_code: {}\n", output.code);
    if !output.stdout.trim().is_empty() {
        text.push_str("--- stdout ---\n");
        text.push_str(&output.stdout);
        if !text.ends_with('\n') {
            text.push('\n');
        }
    }
    if !output.stderr.trim().is_empty() {
        text.push_str("--- stderr ---\n");
        text.push_str(&output.stderr);
    }
    Ok(text_result(text, output.code != 0))
}

fn project_version() -> String {
    let p: PathBuf = repo_root().join("VERSION");
    std::fs::read_to_string(&p)
        .map(|s| s.trim().to_string())
        .ok()
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "v0.0.0-dev".to_string())
}
