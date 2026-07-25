#!/usr/bin/env bash
# Negative test: truncate an encrypted BYTC1 container and prove the loader
# fails cleanly instead of parsing garbage or crashing.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/php/src"
SO="$SRC/modules/opdump.so"
PHP_BIN="${PHP_BIN:-php8.4}"
KEY="${BYTECODE_KEY:-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef}"

if [[ ! -f "$SO" ]]; then
  echo "opdump.so not built -- run: phpize8.4 && ./configure --with-php-config=/usr/bin/php-config8.4 && make" >&2
  exit 1
fi

OUT="$(mktemp -d)"
DECOY="$(mktemp --suffix=.php)"
TRUNCATED="$(mktemp --suffix=.php)"
trap 'rm -f "$DECOY" "$TRUNCATED"; rm -rf "$OUT"' EXIT

printf '<?php this is not valid php syntax !!! %%%%%%\n' > "$DECOY"

BYTECODE_KEY="$KEY" PHP_BIN="$PHP_BIN" "$ROOT/php/bin/bytecode-dump" "$ROOT/php/tests/rung1.php" "$OUT" >/dev/null

size="$(wc -c < "$OUT/rung1.php" | tr -d ' ')"
if [[ "$size" -le 10 ]]; then
  echo "container unexpectedly too small to truncate: $size" >&2
  exit 1
fi

head -c "$((size - 10))" "$OUT/rung1.php" > "$TRUNCATED"

set +e
output="$(
  BYTECODE_KEY="$KEY" OPDUMP_MODE=load OPDUMP_IN="$TRUNCATED" \
    "$PHP_BIN" -n -d extension="$SO" -f "$DECOY" 2>&1
)"
status=$?
set -e

if [[ "$status" -eq 0 ]]; then
  echo "corrupt container unexpectedly loaded successfully" >&2
  echo "$output" >&2
  exit 1
fi

if ! printf '%s\n' "$output" | grep -Eq 'malformed BYTC ciphertext|truncated|authentication/decryption failed'; then
  echo "corrupt container failed without expected parser/decrypt error" >&2
  echo "$output" >&2
  exit 1
fi

echo "corrupt container: PASS"
