#!/usr/bin/env bash
# Default vendor-secret key file smoke: bytecode-dump auto-creates the key file
# when no explicit key is provided, and bytecode-keygen can regenerate it.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/php/src"
SO="$SRC/modules/opdump.so"
PHP_BIN="${PHP_BIN:-php8.4}"

if [[ ! -f "$SO" ]]; then
  echo "opdump.so not built -- run: phpize8.4 && ./configure --with-php-config=/usr/bin/php-config8.4 && make" >&2
  exit 1
fi

TREE="$(mktemp -d)"
OUT="$(mktemp -d)"
KEY_FILE="$(mktemp -u)"
trap 'rm -rf "$TREE" "$OUT"; rm -f "$KEY_FILE"' EXIT

cat > "$TREE/index.php" <<'PHP'
<?php
echo "vendor-secret-default\n";
PHP

BYTECODE_VENDOR_KEY_FILE="$KEY_FILE" PHP_BIN="$PHP_BIN" "$ROOT/php/bin/bytecode-dump" "$TREE" "$OUT" >/dev/null
if [[ ! -f "$KEY_FILE" ]]; then
  echo "vendor key file was not auto-created" >&2
  exit 1
fi
KEY="$(tr -d '\r\n' < "$KEY_FILE")"
if [[ ! "$KEY" =~ ^[0-9a-fA-F]{64}$ ]]; then
  echo "vendor key file does not contain 64 hex chars" >&2
  exit 1
fi

BYTECODE_VENDOR_KEY="$KEY" "$PHP_BIN" "$ROOT/php/bin/bytecode-verify" "$OUT/bytecode.manifest.json" >/dev/null
printf '<?php broken !!!\n' > "$TREE/index.php"
output="$(
  BYTECODE_VENDOR_KEY="$KEY" OPDUMP_MODE=load-tree OPDUMP_MAP="$OUT/bytecode.map" \
    "$PHP_BIN" -n -d extension="$SO" -f "$TREE/index.php"
)"
if [[ "$(printf '%s\n' "$output" | tail -n 1)" != "vendor-secret-default" ]]; then
  echo "unexpected output: $output" >&2
  exit 1
fi

OLD_KEY="$KEY"
"$PHP_BIN" "$ROOT/php/bin/bytecode-keygen" --out "$KEY_FILE" --force >/dev/null
NEW_KEY="$(tr -d '\r\n' < "$KEY_FILE")"
if [[ ! "$NEW_KEY" =~ ^[0-9a-fA-F]{64}$ || "$NEW_KEY" == "$OLD_KEY" ]]; then
  echo "vendor key regeneration did not produce a new 64-hex key" >&2
  exit 1
fi

echo "vendor-secret default: PASS"
