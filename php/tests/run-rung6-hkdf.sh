#!/usr/bin/env bash
# Rung 6: HKDF-derived per-container keys (BYTC2). bytecode-dump/bytecode-pack
# now emit BYTC2 containers carrying a random key_id salt; BYTECODE_KEY is
# treated as HKDF-SHA256 input keying material rather than the raw AES key
# directly. Confirms the round trip still works, that bytecode-info reports
# container_version 2, and that a corrupted key_id fails GCM authentication
# before any payload parsing -- the same failure mode a wrong key produces
# today (see run-corrupt-container.sh).
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
CORRUPT="$(mktemp --suffix=.bytc)"
trap 'rm -f "$DECOY" "$CORRUPT"; rm -rf "$OUT"' EXIT

printf '<?php this is not valid php syntax !!! %%%%%%\n' > "$DECOY"

BYTECODE_KEY="$KEY" PHP_BIN="$PHP_BIN" "$ROOT/php/bin/bytecode-dump" "$ROOT/php/tests/rung1.php" "$OUT" >/dev/null

echo "--- bytecode-info reports BYTC2 ---"
info="$("$ROOT/php/bin/bytecode-info" "$OUT/rung1.php.bytc")"
echo "$info"
if ! grep -q '^container_version: 2$' <<<"$info"; then
  echo "expected container_version 2, bytecode-dump should default to BYTC2" >&2
  exit 1
fi
if ! grep -q '^key_id: ' <<<"$info"; then
  echo "expected a key_id field on a BYTC2 container" >&2
  exit 1
fi
header_size="$(grep '^header_size: ' <<<"$info" | cut -d' ' -f2)"

echo
echo "--- round trip: load rung1 BYTC2 (pointed at invalid-syntax decoy file) ---"
BYTECODE_KEY="$KEY" OPDUMP_MODE=load OPDUMP_IN="$OUT/rung1.php.bytc" "$PHP_BIN" -n -d extension="$SO" -f "$DECOY"

echo
echo "--- corrupted key_id byte fails closed before payload parsing ---"
cp "$OUT/rung1.php.bytc" "$CORRUPT"
"$PHP_BIN" -r '
$path = $argv[1];
$offset = (int) $argv[2];
$data = file_get_contents($path);
$data[$offset] = chr(ord($data[$offset]) ^ 0xFF);
file_put_contents($path, $data);
' "$CORRUPT" "$((header_size - 1))"

set +e
output="$(
  BYTECODE_KEY="$KEY" OPDUMP_MODE=load OPDUMP_IN="$CORRUPT" \
    "$PHP_BIN" -n -d extension="$SO" -f "$DECOY" 2>&1
)"
status=$?
set -e

if [[ "$status" -eq 0 ]]; then
  echo "corrupted key_id unexpectedly loaded successfully" >&2
  echo "$output" >&2
  exit 1
fi
if ! grep -q 'BYTC authentication/decryption failed' <<<"$output"; then
  echo "corrupted key_id failed without the expected authentication error" >&2
  echo "$output" >&2
  exit 1
fi

echo
echo "rung 6 hkdf: PASS"
