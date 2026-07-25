#!/usr/bin/env bash
# Rung 7: license-mode key wrapping. bytecode-dump, given
# BYTECODE_LICENSE_PUBKEY, generates one random DEK per run, RSA-OAEP-SHA256
# wraps it into bytecode.license.json, and every container in that run is
# HKDF-derived from that DEK instead of a shared BYTECODE_KEY secret. The
# loader unwraps the DEK via OPDUMP_LICENSE_KEY_FILE (the matching private
# key) and never needs BYTECODE_KEY/OPDUMP_KEY at all. Confirms the round
# trip works via both explicit OPDUMP_LICENSE_FILE and the OPDUMP_MAP-derived
# default path (load-tree), and that the wrong private key fails closed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/php/src"
SO="$SRC/modules/opdump.so"
PHP_BIN="${PHP_BIN:-php8.4}"

if [[ ! -f "$SO" ]]; then
  echo "opdump.so not built -- run: phpize8.4 && ./configure --with-php-config=/usr/bin/php-config8.4 && make" >&2
  exit 1
fi

LICDIR="$(mktemp -d)"
WRONGLICDIR="$(mktemp -d)"
TREE="$(mktemp -d)"
OUT="$(mktemp -d)"
DECOY="$(mktemp --suffix=.php)"
trap 'rm -f "$DECOY"; rm -rf "$LICDIR" "$WRONGLICDIR" "$TREE" "$OUT"' EXIT

printf '<?php this is not valid php syntax !!! %%%%%%\n' > "$DECOY"

PHP_BIN="$PHP_BIN" "$ROOT/php/bin/bytecode-license-keygen" "$LICDIR" >/dev/null
PHP_BIN="$PHP_BIN" "$ROOT/php/bin/bytecode-license-keygen" "$WRONGLICDIR" >/dev/null

cp "$ROOT/php/tests/rung1.php" "$TREE/rung1.php"

BYTECODE_LICENSE_PUBKEY="$LICDIR/license.pub.pem" PHP_BIN="$PHP_BIN" \
  "$ROOT/php/bin/bytecode-dump" "$TREE/rung1.php" "$OUT" >/dev/null

if [[ ! -f "$OUT/bytecode.license.json" ]]; then
  echo "expected bytecode.license.json when BYTECODE_LICENSE_PUBKEY is set" >&2
  exit 1
fi
if ! grep -q '"alg": "RSA-OAEP-SHA256"' "$OUT/bytecode.license.json"; then
  echo "bytecode.license.json missing expected alg field" >&2
  exit 1
fi

echo "--- round trip: explicit OPDUMP_LICENSE_FILE ---"
OPDUMP_LICENSE_KEY_FILE="$LICDIR/license.key.pem" OPDUMP_LICENSE_FILE="$OUT/bytecode.license.json" \
  OPDUMP_MODE=load OPDUMP_IN="$OUT/rung1.php" \
  "$PHP_BIN" -n -d extension="$SO" -f "$DECOY"

echo
echo "--- round trip: load-tree derives bytecode.license.json from OPDUMP_MAP dir ---"
printf '<?php this is not valid php syntax !!! %%%%%%\n' > "$TREE/rung1.php"
output="$(
  OPDUMP_LICENSE_KEY_FILE="$LICDIR/license.key.pem" OPDUMP_MODE=load-tree OPDUMP_MAP="$OUT/bytecode.map" \
    "$PHP_BIN" -n -d extension="$SO" -f "$TREE/rung1.php"
)"
if [[ "$(printf '%s\n' "$output" | tail -n 1)" != "hello world" ]]; then
  echo "unexpected output: $output" >&2
  exit 1
fi

echo
echo "--- wrong private key fails closed ---"
set +e
output="$(
  OPDUMP_LICENSE_KEY_FILE="$WRONGLICDIR/license.key.pem" OPDUMP_LICENSE_FILE="$OUT/bytecode.license.json" \
    OPDUMP_MODE=load OPDUMP_IN="$OUT/rung1.php" \
    "$PHP_BIN" -n -d extension="$SO" -f "$DECOY" 2>&1
)"
status=$?
set -e

if [[ "$status" -eq 0 ]]; then
  echo "wrong private key unexpectedly loaded successfully" >&2
  echo "$output" >&2
  exit 1
fi
if ! grep -q 'no key material available for BYTC2 load' <<<"$output"; then
  echo "wrong private key failed without the expected diagnostic" >&2
  echo "$output" >&2
  exit 1
fi

echo
echo "rung 7 license: PASS"
