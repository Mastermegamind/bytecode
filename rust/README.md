# Rust tools (`rust/`)

Rust reimplementations of the standalone crypto/format CLIs that today live in
[`php/bin`](../php/bin). They are **byte-compatible** with the PHP originals:
same container format, same key derivation, same checks, same success output,
and the same exit codes — so a build produced by either side verifies on the
other.

These tools never touch the live PHP engine (that stays in the C extension
`php/src/opdump.c`). They only read/write the `BYTC` container format and do
crypto, which makes them a good fit for a single, dependency-free static binary.

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
