//! `bytecode-api` — a small local HTTP/1.1 control server (std-only, no async
//! runtime) exposing the bytecode tools for agentic control, matching the CLI
//! and MCP surfaces.
//!
//! Auth is default-deny: BYTECODE_API_TOKEN is REQUIRED and the server refuses
//! to start without it. Every request must send `Authorization: Bearer <token>`.
//! (Env values forwarded to tools are readable via /proc by the same OS user.)
//!
//! Network policy:
//!   * It binds loopback only (127.0.0.1 / ::1). Binding any other address is
//!     refused unless BYTECODE_API_ALLOW_REMOTE=1 is set explicitly.
//!   * It is intended for a single local operator/agent, not a shared network.
//!
//! Endpoints
//!   GET  /health        -> { "ok": true, "version": "..." }
//!   GET  /tools         -> [ { "name", "description" }, ... ]
//!   POST /tools/<name>  body { "args":[..], "stdin":"..", "env":{..} }
//!                       -> { "code":N, "stdout":"..", "stderr":".." }

use std::io::{BufReader, Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::time::Duration;
use std::path::PathBuf;

use bytecode_tools::{ops, repo_root};
use serde_json::{json, Value};

fn main() {
    let addr = std::env::args().nth(1).unwrap_or_else(|| "127.0.0.1:8787".to_string());

    // Default-deny: a bearer token is mandatory.
    let token = match std::env::var("BYTECODE_API_TOKEN") {
        Ok(t) if !t.is_empty() && t.bytes().all(|b| b.is_ascii_graphic()) => t,
        Err(std::env::VarError::NotPresent) => {
            eprintln!("bytecode-api: BYTECODE_API_TOKEN is required (default-deny); set a bearer token before starting");
            std::process::exit(1);
        }
        _ => {
            eprintln!("bytecode-api: BYTECODE_API_TOKEN must be a nonempty ASCII token without spaces");
            std::process::exit(1);
        }
    };

    let allow_remote = std::env::var("BYTECODE_API_ALLOW_REMOTE")
        .map(|v| v == "1").unwrap_or(false);
    let address: SocketAddr = addr.parse().unwrap_or_else(|_| {
        eprintln!("bytecode-api: expected an IP address and port (e.g. 127.0.0.1:8787 or [::1]:8787)");
        std::process::exit(1);
    });
    if !address.ip().is_loopback() && !allow_remote {
        eprintln!("bytecode-api: non-loopback binding requires BYTECODE_API_ALLOW_REMOTE=1");
        std::process::exit(1);
    }

    let listener = match TcpListener::bind(address) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("bytecode-api: cannot bind {addr}: {e}");
            std::process::exit(1);
        }
    };
    eprintln!("bytecode-api listening on http://{} ({} tools, bearer auth required)",
        listener.local_addr().unwrap(), ops::TOOLS.len());

    for stream in listener.incoming() {
        let Ok(stream) = stream else { continue };
        // Serial execution keeps the number of connections and tool processes bounded.
        if let Err(e) = handle(stream, &token) {
            eprintln!("bytecode-api: connection failed: {e}");
        }
    }
}

struct Request {
    method: String,
    path: String,
    auth: Option<String>,
    body: Vec<u8>,
}

const MAX_HEADERS: usize = 16 * 1024;
const MAX_BODY: usize = 64 * 1024 * 1024;

fn bad_request(message: &str) -> std::io::Error {
    std::io::Error::new(std::io::ErrorKind::InvalidData, message)
}

fn read_request(input: impl Read) -> std::io::Result<Request> {
    let mut reader = BufReader::new(input);
    let mut headers = Vec::new();
    // Bound the whole header block, including the request line, before allocation.
    loop {
        let mut byte = [0];
        reader.read_exact(&mut byte)?;
        headers.push(byte[0]);
        if headers.ends_with(b"\r\n\r\n") {
            break;
        }
        if headers.len() >= MAX_HEADERS {
            return Err(bad_request("headers too large"));
        }
    }
    let headers = std::str::from_utf8(&headers)
        .map_err(|_| bad_request("invalid headers"))?;
    let mut lines = headers.split("\r\n");
    let parts: Vec<_> = lines.next().unwrap_or("").split_whitespace().collect();
    if parts.len() != 3 || !matches!(parts[2], "HTTP/1.0" | "HTTP/1.1")
        || !parts[1].starts_with('/') {
        return Err(bad_request("invalid request line"));
    }
    let mut content_length = None;
    let mut auth = None;
    for line in lines {
        if line.is_empty() { break; }
        let (key, value) = line.split_once(':').ok_or_else(|| bad_request("invalid header"))?;
        if key.is_empty() || !key.bytes().all(|b| b.is_ascii_alphanumeric() || b"!#$%&'*+-.^_`|~".contains(&b)) {
            return Err(bad_request("invalid header name"));
        }
        let value = value.trim();
        match key.to_ascii_lowercase().as_str() {
            "content-length" => {
                if content_length.is_some() || value.is_empty() || !value.bytes().all(|b| b.is_ascii_digit()) {
                    return Err(bad_request("invalid or duplicate content-length"));
                }
                let length: usize = value.parse().map_err(|_| bad_request("invalid content-length"))?;
                if length > MAX_BODY { return Err(bad_request("body too large")); }
                content_length = Some(length);
            }
            "authorization" => {
                if auth.is_some() { return Err(bad_request("duplicate authorization")); }
                auth = Some(value.to_string());
            }
            "transfer-encoding" | "expect" => return Err(bad_request("unsupported request framing")),
            _ => {}
        }
    }
    let mut body = vec![0; content_length.unwrap_or(0)];
    reader.read_exact(&mut body)?;
    Ok(Request { method: parts[0].into(), path: parts[1].into(), auth, body })
}

fn handle(mut stream: TcpStream, token: &str) -> std::io::Result<()> {
    stream.set_read_timeout(Some(Duration::from_secs(10)))?;
    stream.set_write_timeout(Some(Duration::from_secs(10)))?;
    let req = match read_request(&stream) {
        Ok(r) => r,
        Err(_) => return respond(&mut stream, 400, &json!({ "error": "bad request" })),
    };

    let authorized = req
        .auth
        .as_deref()
        .and_then(|a| a.split_once(' '))
        .filter(|(scheme, _)| scheme.eq_ignore_ascii_case("Bearer"))
        .map(|(_, supplied)| bytecode_tools::ct_eq(supplied, token))
        .unwrap_or(false);
    if !authorized {
        return respond(&mut stream, 401, &json!({ "error": "unauthorized" }));
    }

    match (req.method.as_str(), req.path.as_str()) {
        ("GET", "/health") => respond(&mut stream, 200, &json!({ "ok": true, "version": project_version() })),
        ("GET", "/tools") => {
            let mut tools: Vec<Value> = ops::TOOLS
                .iter()
                .map(|(n, d)| json!({ "name": n, "description": d, "help": ops::tool_help(n) }))
                .collect();
            tools.push(json!({
                "name": "encode",
                "description": "Pipeline: scan? -> dump -> verify?. Body: {\"dump_args\":[],\"scan_paths\":[],\"verify_dir\":\"\",\"decrypt_test\":false,\"env\":{}}"
            }));
            respond(&mut stream, 200, &Value::Array(tools))
        }
        ("POST", "/tools/encode") => encode_endpoint(&mut stream, &req.body),
        ("POST", p) if p.starts_with("/tools/") => run_tool_endpoint(&mut stream, &p["/tools/".len()..], &req.body),
        _ => respond(&mut stream, 404, &json!({ "error": "not found" })),
    }
}

fn run_tool_endpoint(stream: &mut TcpStream, name: &str, body: &[u8]) -> std::io::Result<()> {
    if !ops::is_tool(name) {
        return respond(stream, 404, &json!({ "error": format!("unknown tool: {name}") }));
    }
    let v: Value = if body.is_empty() {
        json!({})
    } else {
        match serde_json::from_slice(body) {
            Ok(v) => v,
            Err(e) => return respond(stream, 400, &json!({ "error": format!("invalid JSON body: {e}") })),
        }
    };

    let (args, stdin, envs) = match tool_input(&v) {
        Ok(input) => input,
        Err(e) => return respond(stream, 400, &json!({ "error": e })),
    };

    match ops::run_tool(name, &args, stdin.as_deref().map(str::as_bytes), &envs) {
        Ok(o) => respond(stream, 200, &json!({ "code": o.code, "stdout": o.stdout, "stderr": o.stderr })),
        Err(e) => respond(stream, 500, &json!({ "error": e })),
    }
}

fn encode_endpoint(stream: &mut TcpStream, body: &[u8]) -> std::io::Result<()> {
    let v: Value = if body.is_empty() {
        json!({})
    } else {
        match serde_json::from_slice(body) {
            Ok(v) => v,
            Err(e) => return respond(stream, 400, &json!({ "error": format!("invalid JSON body: {e}") })),
        }
    };
    let string_array = |key: &str| -> Result<Vec<String>, String> {
        match v.get(key) {
            None | Some(Value::Null) => Ok(Vec::new()),
            Some(Value::Array(a)) => a
                .iter()
                .map(|x| x.as_str().map(str::to_string).ok_or_else(|| format!("{key} must be strings")))
                .collect(),
            _ => Err(format!("{key} must be an array of strings")),
        }
    };
    let dump_args = match string_array("dump_args") {
        Ok(a) => a,
        Err(e) => return respond(stream, 400, &json!({ "error": e })),
    };
    let scan_paths = match string_array("scan_paths") {
        Ok(a) => a,
        Err(e) => return respond(stream, 400, &json!({ "error": e })),
    };
    let verify_dir = v.get("verify_dir").and_then(Value::as_str).map(str::to_string);
    let decrypt_test = v.get("decrypt_test").and_then(Value::as_bool).unwrap_or(false);

    let mut envs = Vec::new();
    if let Some(obj) = v.get("env") {
        match obj.as_object() {
            Some(m) => {
                for (k, val) in m {
                    let Some(val) = val.as_str() else {
                        return respond(stream, 400, &json!({ "error": "env values must be strings" }));
                    };
                    if k.is_empty() || k.contains(['=', '\0']) || val.contains('\0') {
                        return respond(stream, 400, &json!({ "error": "invalid environment entry" }));
                    }
                    envs.push((k.clone(), val.to_string()));
                }
            }
            None => return respond(stream, 400, &json!({ "error": "env must be an object of strings" })),
        }
    }

    match ops::encode(&dump_args, &scan_paths, verify_dir.as_deref(), decrypt_test, &envs) {
        Ok(o) => respond(stream, 200, &json!({ "code": o.code, "stdout": o.stdout, "stderr": o.stderr })),
        Err(e) => respond(stream, 500, &json!({ "error": e })),
    }
}

fn respond(stream: &mut TcpStream, status: u16, body: &Value) -> std::io::Result<()> {
    let reason = match status {
        200 => "OK",
        400 => "Bad Request",
        401 => "Unauthorized",
        404 => "Not Found",
        500 => "Internal Server Error",
        _ => "OK",
    };
    let payload = serde_json::to_vec(body)?;
    let challenge = if status == 401 { "WWW-Authenticate: Bearer\r\n" } else { "" };
    write!(
        stream,
        "HTTP/1.1 {status} {reason}\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n{challenge}Content-Length: {}\r\nConnection: close\r\n\r\n",
        payload.len()
    )?;
    stream.write_all(&payload)?;
    stream.flush()
}

type ToolInput = (Vec<String>, Option<String>, Vec<(String, String)>);

fn tool_input(v: &Value) -> Result<ToolInput, String> {
    let object = v.as_object().ok_or("body must be a JSON object")?;
    let mut args = Vec::new();
    if let Some(value) = object.get("args") {
        for arg in value.as_array().ok_or("args must be an array of strings")? {
            let arg = arg.as_str().ok_or("args must contain only strings")?;
            if arg.contains('\0') { return Err("args cannot contain NUL".into()); }
            args.push(arg.to_string());
        }
    }
    let stdin = object.get("stdin").map(|s| s.as_str().map(str::to_string)
        .ok_or_else(|| "stdin must be a string".to_string())).transpose()?;
    let mut envs = Vec::new();
    if let Some(value) = object.get("env") {
        for (key, value) in value.as_object().ok_or("env must be an object of strings")? {
            let value = value.as_str().ok_or("env values must be strings")?;
            if key.is_empty() || key.contains(['=', '\0']) || value.contains('\0') {
                return Err("invalid environment entry".into());
            }
            envs.push((key.clone(), value.to_string()));
        }
    }
    Ok((args, stdin, envs))
}

fn project_version() -> String {
    let p: PathBuf = repo_root().join("VERSION");
    std::fs::read_to_string(&p)
        .map(|s| s.trim().to_string())
        .ok()
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "v0.0.0-dev".to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_body_and_case_insensitive_headers() {
        let req = read_request(&b"POST /tools/version HTTP/1.1\r\ncOnTeNt-LeNgTh: 2\r\nauthorization: Bearer test\r\n\r\n{}"[..]).unwrap();
        assert_eq!(req.path, "/tools/version");
        assert_eq!(req.auth.as_deref(), Some("Bearer test"));
        assert_eq!(req.body, b"{}");
    }

    #[test]
    fn rejects_ambiguous_truncated_and_oversized_requests() {
        for request in [
            "POST /tools/version HTTP/1.1\r\nContent-Length: 0\r\nContent-Length: 2\r\n\r\n{}",
            "POST /tools/version HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
            "POST /tools/version HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
            "POST /tools/version HTTP/1.1\r\nContent-Length: 67108865\r\n\r\n",
            "POST /tools/version HTTP/1.1\r\nContent-Length: 2\r\n\r\n{",
            "GET /health HTTP/1.1\r\nAuthorization: a\r\nAuthorization: b\r\n\r\n",
            "GET /health HTTP/1.1\r\n",
        ] {
            assert!(read_request(request.as_bytes()).is_err(), "{request:?}");
        }
        assert!(read_request(vec![b'x'; MAX_HEADERS].as_slice()).is_err());
    }

    #[test]
    fn validates_execution_input_without_silently_dropping_values() {
        for invalid in [
            json!(null), json!([]), json!({"args": "--help"}),
            json!({"args": ["--help", 1]}), json!({"stdin": 1}),
            json!({"env": {"KEY": false}}), json!({"env": {"BAD=KEY": "x"}}),
            json!({"args": ["\u{0}"]}),
        ] {
            assert!(tool_input(&invalid).is_err(), "{invalid}");
        }
        let (args, stdin, env) = tool_input(&json!({
            "args": ["--help"], "stdin": "input", "env": {"BYTECODE_KEY": "test"}
        })).unwrap();
        assert_eq!(args, ["--help"]);
        assert_eq!(stdin.as_deref(), Some("input"));
        assert_eq!(env, [("BYTECODE_KEY".into(), "test".into())]);
        assert!(tool_input(&json!({})).is_ok());
    }
}
