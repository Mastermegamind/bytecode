#!/usr/bin/env bash
# PHP <-> Rust byte-compatibility parity.
#
# Proves the Rust tool binaries and the PHP tools agree: a container packed by
# one verifies under the other, keygen/package-sign round-trip, and tampering is
# rejected by both. Uses a fabricated OPD2 blob, so it needs NO opdump extension
# (CI-friendly) -- only php, openssl and a release build of rust/.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PHP_BIN="${PHP_BIN:-php8.4}"
RUST="$ROOT/rust/target/release"

# Build the Rust tools if needed.
if [[ ! -x "$RUST/bytecode-verify" || ! -x "$RUST/bytecode-pack" ]]; then
  if command -v cargo >/dev/null; then
    ( cd "$ROOT/rust" && cargo build --release )
  else
    echo "rust tools not built and cargo unavailable" >&2
    exit 1
  fi
fi

PHP_VERSION_ID="$("$PHP_BIN" -r 'echo PHP_VERSION_ID;')"
export BYTECODE_ROOT="$ROOT"
export BYTECODE_PHP_VERSION_ID="$PHP_VERSION_ID"
KEY="$("$RUST/bytecode-keygen")"
export BYTECODE_KEY="$KEY"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
printf 'OPD2\x01\x02\x03\x04parity-test-payload' > "$TMP/raw.opd2"

php_tool() { BYTECODE_NO_RUST=1 "$PHP_BIN" "$ROOT/php/bin/bytecode-$1" "${@:2}"; }
rust_tool() { "$RUST/bytecode-$1" "${@:2}"; }

pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1" >&2; exit 1; }

# 1. keygen shape (both 64 hex)
[[ "$(php_tool keygen | tr -d '\n')" =~ ^[0-9a-f]{64}$ ]] || fail "php keygen shape"
[[ "$(rust_tool keygen | tr -d '\n')" =~ ^[0-9a-f]{64}$ ]] || fail "rust keygen shape"
pass "keygen shape"

# 2. cross pack/verify both directions, with decrypt-test
php_tool  pack "$TMP/raw.opd2" "$TMP/php.bytc"
rust_tool pack "$TMP/raw.opd2" "$TMP/rust.bytc"
rust_tool verify --decrypt-test "$TMP/php.bytc"  >/dev/null || fail "rust verify of php-packed"
php_tool  verify --decrypt-test "$TMP/rust.bytc" >/dev/null || fail "php verify of rust-packed"
pass "cross pack/verify (decrypt-test)"

# 3. tamper rejection (flip a ciphertext byte) -- both must fail
cp "$TMP/rust.bytc" "$TMP/tam.bytc"
SIZE="$(stat -c%s "$TMP/tam.bytc")"
printf '\xff' | dd of="$TMP/tam.bytc" bs=1 seek=$((SIZE-1)) count=1 conv=notrunc 2>/dev/null
if rust_tool verify --decrypt-test "$TMP/tam.bytc" >/dev/null 2>&1; then fail "rust accepted tampered"; fi
if php_tool  verify --decrypt-test "$TMP/tam.bytc" >/dev/null 2>&1; then fail "php accepted tampered"; fi
pass "tamper rejected by both"

# 4. package-sign produces identical JSON (modulo the created_at timestamp)
mkdir -p "$TMP/pkg/sub"
printf 'alpha' > "$TMP/pkg/a.txt"
printf 'bravo' > "$TMP/pkg/sub/b.bin"
cp -r "$TMP/pkg" "$TMP/pkg-php"; cp -r "$TMP/pkg" "$TMP/pkg-rust"
php_tool  package-sign "$TMP/pkg-php"  >/dev/null
rust_tool package-sign "$TMP/pkg-rust" >/dev/null
norm() { sed -E 's/"created_at": "[^"]*"/"created_at":"TS"/' "$1"; }
if ! diff <(norm "$TMP/pkg-php/bytecode.package.json") <(norm "$TMP/pkg-rust/bytecode.package.json") >/dev/null; then
  fail "package.json differs between php and rust"
fi
pass "package-sign byte-identical (modulo timestamp)"

echo "rust-parity: all checks passed"
