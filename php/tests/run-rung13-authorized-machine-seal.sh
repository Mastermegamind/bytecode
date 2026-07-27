#!/usr/bin/env bash
# Rung 13: signed vendor-seal machine authorization for shared/vendor-key
# packages. This proves a BYTC tree can be bound to OPDUMP_MACHINE_ID without
# RSA license mode, as long as the package has bytecode.seal.json and the loader
# is configured with the vendor public-key trust anchor.
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

TREE="$(mktemp -d)"
OUT="$(mktemp -d)"
KEYS="$(mktemp -d)"
trap 'rm -rf "$TREE" "$OUT" "$KEYS"' EXIT

cat > "$TREE/main.php" <<'PHP'
<?php
echo "authorized-machine-ok\n";
PHP

"$PHP_BIN" "$ROOT/php/bin/bytecode-vendor-keygen" "$KEYS" >/dev/null
PUB="$KEYS/vendor.sign.pub.pem"
PRIV="$KEYS/vendor.sign.key.pem"

BYTECODE_KEY="$KEY" BYTECODE_VENDOR_SIGN_KEY="$PRIV" PHP_BIN="$PHP_BIN" \
  "$PHP_BIN" "$ROOT/php/bin/bytecode-dump" --machine-id machine-a "$TREE" "$OUT" >/dev/null

BYTECODE_KEY="$KEY" BYTECODE_VENDOR_PUBKEY="$PUB" \
  "$PHP_BIN" "$ROOT/php/bin/bytecode-verify" "$OUT" | grep -q 'Ed25519 signature verified'

echo "--- authorized machine loads ---"
output="$(
  BYTECODE_KEY="$KEY" OPDUMP_VENDOR_PUBKEY_FILE="$PUB" OPDUMP_MACHINE_ID=machine-a \
    OPDUMP_MODE=load-tree OPDUMP_MAP="$OUT/bytecode.map" \
    "$PHP_BIN" -n -d extension="$SO" -f "$TREE/main.php"
)"
if [[ "$(printf '%s\n' "$output" | tail -n 1)" != "authorized-machine-ok" ]]; then
  echo "authorized machine did not run expected code: $output" >&2
  exit 1
fi

echo "--- unauthorized machine is refused ---"
printf '<?php this is not valid php syntax !!! %%%%%%\n' > "$TREE/main.php"
set +e
output="$(
  BYTECODE_KEY="$KEY" OPDUMP_VENDOR_PUBKEY_FILE="$PUB" OPDUMP_MACHINE_ID=machine-b \
    OPDUMP_MODE=load-tree OPDUMP_MAP="$OUT/bytecode.map" \
    "$PHP_BIN" -n -d extension="$SO" -f "$TREE/main.php" 2>&1
)"
status=$?
set -e
if [[ "$status" -eq 0 || "$output" == *authorized-machine-ok* ]]; then
  echo "unauthorized machine unexpectedly loaded protected code" >&2
  echo "$output" >&2
  exit 1
fi
grep -q 'seal policy machine_id does not match' <<<"$output" || {
  echo "missing unauthorized-machine diagnostic" >&2
  echo "$output" >&2
  exit 1
}

echo "--- tampered policy is refused by policy_sha256 ---"
"$PHP_BIN" -r '$p=$argv[1]; $j=file_get_contents($p); $j=str_replace("\"machine_id\": \"machine-a\"", "\"machine_id\": \"machine-b\"", $j); file_put_contents($p, $j);' "$OUT/bytecode.seal.json"
set +e
output="$(
  BYTECODE_KEY="$KEY" OPDUMP_VENDOR_PUBKEY_FILE="$PUB" OPDUMP_MACHINE_ID=machine-b \
    OPDUMP_MODE=load-tree OPDUMP_MAP="$OUT/bytecode.map" \
    "$PHP_BIN" -n -d extension="$SO" -f "$TREE/main.php" 2>&1
)"
status=$?
set -e
if [[ "$status" -eq 0 || "$output" == *authorized-machine-ok* ]]; then
  echo "tampered seal policy unexpectedly loaded protected code" >&2
  echo "$output" >&2
  exit 1
fi
grep -q 'policy digest does not match' <<<"$output" || {
  echo "missing tampered-policy diagnostic" >&2
  echo "$output" >&2
  exit 1
}

echo "rung 13 authorized-machine-seal: PASS"
