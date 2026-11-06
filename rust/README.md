# Rust tools (`rust/`)

Rust reimplementations of the standalone crypto/format CLIs that today live in
[`php/bin`](../php/bin). They are **byte-compatible** with the PHP originals:
same container format, same key derivation, same checks, same success output,
and the same exit codes — so a build produced by either side verifies on the
other.

These tools never touch the live PHP engine (that stays in the C extension
`php/src/opdump.c`). They only read/write the `BYTC` container format and do
crypto, which makes them a good fit for a single, dependency-free static binary.

## Architecture: one core, many front-ends

```
          src/lib.rs  (+ src/ops.rs: operations & the tool registry)
                 |
   ┌─────────────┼───────────────┬───────────────┬──────────────┐
   │             │               │               │              │
 per-tool      FFI             CLI             MCP            HTTP API
 binaries   (cdylib,        `bytecode`     `bytecode-mcp`  `bytecode-api`
 (verify,   src/ffi.rs)    (src/bin/       (stdio JSON-    (std-only
  pack,     for Flutter     bytecode.rs)    RPC 2.0)        HTTP/1.1)
  keygen…)  in-process
```

`src/ops.rs` holds the shared operation functions (`keygen_hex`, `pack`,
`verify_one`) **and** `TOOLS` — the registry of every `bytecode-*` tool. The
CLI, MCP and API all drive the full registry (no capability subset): each tool
call runs `php php/bin/bytecode-<tool>`, which self-dispatches to the Rust
binary when one is present, so every adapter is Rust-first *and* complete,
including the PHP-only tools (`dump`, `obfuscate`, `deploy`, `doctor`, …).

## Status

| Tool | Rust | PHP fallback |
|------|------|--------------|
| `bytecode-verify` | ✅ `src/bin/bytecode-verify.rs` | `php/bin/bytecode-verify` |
| `bytecode-pack` | ✅ `src/bin/bytecode-pack.rs` | `php/bin/bytecode-pack` |
| `bytecode-keygen` | ✅ `src/bin/bytecode-keygen.rs` | `php/bin/bytecode-keygen` |
| `bytecode-vendor-keygen` | ✅ `src/bin/bytecode-vendor-keygen.rs` | `php/bin/bytecode-vendor-keygen` |
| `bytecode-license-keygen` | ✅ `src/bin/bytecode-license-keygen.rs` | `php/bin/bytecode-license-keygen` |
| `bytecode-package-sign` | ✅ `src/bin/bytecode-package-sign.rs` | `php/bin/bytecode-package-sign` |
| `bytecode-scan` | PHP-only (see note) | `php/bin/bytecode-scan` |

Shared logic (container parsing, HKDF/HMAC/AES-GCM, key resolution, the openssl
shell-out) lives in `src/lib.rs` so the tools can reuse it.

### Why `bytecode-scan` stays PHP-only

`bytecode-scan` is intentionally **not** ported. Unlike the crypto/format tools,
its core depends on PHP's own lexer — it calls `token_get_all()` and branches on
`T_VARIABLE` / `T_STRING` / `T_CONSTANT_ENCAPSED_STRING` / `T_NEW` (only the 12
regex checks are lexer-independent). A faithful Rust port would require
hand-writing a PHP lexer (inline-HTML boundaries, string interpolation,
heredoc/nowdoc, `#[` attributes), which could not be guaranteed byte-identical
to PHP's tokenizer — and for a preflight compat/security scanner, divergence
means false or missed warnings. The PHP tool has no dispatch shim, so it always
runs as the canonical implementation.

## Build

```sh
cd rust
cargo build --release
# -> rust/target/release/bytecode-verify
```

Requires a system `openssl` CLI for the Ed25519 vendor-seal check (the PHP tool
uses it too). Everything else is pure Rust.

## Rust-first dispatch (how the fallback works)

The command name (`php/bin/bytecode-verify`) stays a valid PHP script, because
callers invoke it as `php php/bin/bytecode-verify …`. On startup that script
looks for a compiled Rust binary and, if found, hands off to it; otherwise it
runs the PHP implementation unchanged. Resolution order:

1. `$BYTECODE_<TOOL>_RS` — explicit path (e.g. `BYTECODE_VERIFY_RS`, `BYTECODE_PACK_RS`)
2. `$BYTECODE_ROOT/rust/target/release/<tool>` (or repo root)
3. `<php/bin>/<tool>-rs` (packaged alongside)

Controls:

- `BYTECODE_NO_RUST=1` — force the PHP implementation.
- The wrapper exports `BYTECODE_PHP_VERSION_ID` (the running interpreter's id,
  which the container check pins to) and `BYTECODE_ROOT` to the Rust binary, so
  the Rust and PHP paths resolve the same PHP version and the same default
  vendor-key / public-key locations.

Running the Rust binary directly (no PHP): set `BYTECODE_PHP_VERSION_ID`
(or have `php` on `PATH` for auto-detection) and, if needed, `BYTECODE_ROOT`.

## Parity testing

Any build verifies identically under both implementations:

```sh
# PHP path
BYTECODE_NO_RUST=1 BYTECODE_KEY=$KEY php php/bin/bytecode-verify --decrypt-test out/
# Rust path (default when built)
BYTECODE_KEY=$KEY php php/bin/bytecode-verify --decrypt-test out/
```

## Front-ends

### FFI (for the Flutter desktop app)

`cargo build --release` produces `libbytecode_tools.{so,dylib,dll}` (cdylib) and
`libbytecode_tools.a` (staticlib). The C ABI is declared in
[`include/bytecode_tools.h`](include/bytecode_tools.h):

```c
char   *bt_keygen_hex(void);                 // free with bt_string_free
int     bt_pack(const char *raw, const char *out, uint32_t php_version_id);
int     bt_verify_one(const char *path, int decrypt_test,
                      uint32_t want_php_version_id, char **out_json);
int64_t bt_detect_php_version_id(void);
const char *bt_last_error(void);             // thread-local, last failure
void    bt_string_free(char *s);
```

This lets the Flutter app run keygen/pack/verify **in-process** (no subprocess,
no PHP) for the per-file paths. A Dart binding sketch:

```dart
import 'dart:ffi';
import 'package:ffi/ffi.dart';

final lib = DynamicLibrary.open('libbytecode_tools.so');
final keygen = lib.lookupFunction<Pointer<Utf8> Function(),
                                  Pointer<Utf8> Function()>('bt_keygen_hex');
final free = lib.lookupFunction<Void Function(Pointer<Utf8>),
                                void Function(Pointer<Utf8>)>('bt_string_free');

final p = keygen();
final key = p.toDartString();
free(p);
```

Full manifest/seal verification stays in the `bytecode-verify` binary; the FFI
covers single-container verify (`bt_verify_one`).

### CLI — `bytecode`

One front door over every tool (args pass through verbatim):

```sh
bytecode keygen
bytecode pack app.opd2 app.bytc
bytecode verify --decrypt-test out/
bytecode dump ...            # the PHP encoder, reachable too
bytecode encode --verify-dir out --decrypt-test -- --out out src/  # scan?->dump->verify?
bytecode mcp                 # launch the MCP server
bytecode api 127.0.0.1:8787  # launch the HTTP API
bytecode --help              # lists all tools
```

### MCP — `bytecode-mcp` (agentic control)

A Model Context Protocol server over stdio (newline-delimited JSON-RPC 2.0).
Every tool in the registry is exposed as an MCP tool whose input is
`{ "args": [..], "stdin"?: "..", "env"?: { KEY: VALUE } }` — the tool's full CLI
surface. Register it with an MCP client, e.g.:

```json
{ "mcpServers": { "bytecode": { "command": "/path/to/bytecode-mcp" } } }
```

Each tool's `tools/list` description includes its cached `--help`. A high-level
`encode` tool runs the whole pipeline in one call
(`{ "dump_args":[..], "scan_paths"?:[..], "verify_dir"?:"..", "decrypt_test"?:bool, "env"?:{..} }`).
Adapters only forward `BYTECODE_*` / `OPDUMP_*` / `PHP_BIN` env to tools; other
keys are rejected (and forwarded values are visible via `/proc` to the same OS
user). Implements `initialize`, `tools/list`, `tools/call`, `ping`.

### HTTP API — `bytecode-api` (locked down)

A std-only local HTTP server. **Requires** `BYTECODE_API_TOKEN` (refuses to
start without it) and binds loopback only (override with
`BYTECODE_API_ALLOW_REMOTE=1`). Every request needs `Authorization: Bearer
<token>`.

```sh
BYTECODE_API_TOKEN=secret bytecode-api 127.0.0.1:8787 &

curl -H "Authorization: Bearer secret" localhost:8787/tools
curl -H "Authorization: Bearer secret" -X POST localhost:8787/tools/verify \
     -d '{"args":["--decrypt-test","out/"],"env":{"BYTECODE_KEY":"<64hex>"}}'
```

Endpoints: `GET /health`, `GET /tools` (incl. per-tool `--help`), `POST
/tools/<name>` → `{code, stdout, stderr}`, and `POST /tools/encode`
(`{dump_args, scan_paths, verify_dir, decrypt_test, env}`) for the pipeline. The
same `BYTECODE_*`/`OPDUMP_*`/`PHP_BIN` env allowlist applies.

The on-disk format/crypto contract is specified in
[`../docs/CONTAINER-FORMAT.md`](../docs/CONTAINER-FORMAT.md); PHP↔Rust parity is
enforced by `php/tests/run-rust-parity.sh` and the `rust-tools` CI workflow.
