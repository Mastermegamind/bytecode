#!/usr/bin/env bash
# Rung 10: vendor seal (Ed25519). bytecode.manifest.sig is an HMAC keyed by the
# same IKM the running server already holds -- so a party that possesses the
# key (in license mode, the operator who unwraps the DEK) can edit the package
# and simply re-HMAC manifest.sig, and license.json was never covered by it at
# all. The vendor seal closes that gap: bytecode-dump signs the digests of
# manifest+map+license with the vendor's Ed25519 PRIVATE key, and a loader
# configured with the matching PUBLIC key (compiled in, or via
# OPDUMP_VENDOR_PUBKEY_FILE) refuses any package whose files were altered.
#
# This confirms: (1) a sealed, untampered tree loads under the anchor;
# (2) a loader with NO anchor still loads it (backward compatible);
# (3) tampering the manifest AND forging a fresh, valid manifest.sig with the
#     shared key is STILL refused by the seal -- with the source destroyed
#     first so a fallback to real compilation cannot mask a broken check.
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
if ! command -v openssl >/dev/null 2>&1; then
  echo "openssl CLI not found -- required for Ed25519 sealing" >&2
  exit 1
fi

TREE="$(mktemp -d)"
OUT="$(mktemp -d)"
KEYS="$(mktemp -d)"
trap 'rm -rf "$TREE" "$OUT" "$KEYS"' EXIT

cat > "$TREE/lib.php" <<'PHP'
<?php
function tree_message(string $name = 'world'): string
{
    return "tree:$name";
}
PHP

cat > "$TREE/main.php" <<'PHP'
<?php
require __DIR__ . '/lib.php';
echo tree_message('loader'), "\n";
PHP

"$PHP_BIN" "$ROOT/php/bin/bytecode-vendor-keygen" "$KEYS" >/dev/null
PUB="$KEYS/vendor.sign.pub.pem"
PRIV="$KEYS/vendor.sign.key.pem"

BYTECODE_KEY="$KEY" BYTECODE_VENDOR_SIGN_KEY="$PRIV" PHP_BIN="$PHP_BIN" \
  "$ROOT/php/bin/bytecode-dump" "$TREE" "$OUT" >/dev/null

if [[ ! -f "$OUT/bytecode.seal.json" ]]; then
  echo "expected bytecode.seal.json next to bytecode.manifest.json" >&2
  exit 1
fi

echo "--- bytecode-verify checks the seal end-to-end ---"
BYTECODE_KEY="$KEY" BYTECODE_VENDOR_PUBKEY="$PUB" \
  "$PHP_BIN" "$ROOT/php/bin/bytecode-verify" "$OUT" | grep -q 'Ed25519 signature verified' \
  || { echo "bytecode-verify did not confirm the Ed25519 seal" >&2; exit 1; }

echo "--- sealed tree loads under the vendor anchor ---"
output="$(
  BYTECODE_KEY="$KEY" OPDUMP_VENDOR_PUBKEY_FILE="$PUB" \
    OPDUMP_MODE=load-tree OPDUMP_MAP="$OUT/bytecode.map" \
    "$PHP_BIN" -n -d extension="$SO" -f "$TREE/main.php"
)"
if [[ "$(printf '%s\n' "$output" | tail -n 1)" != "tree:loader" ]]; then
  echo "sealed tree failed to load under anchor: $output" >&2
  exit 1
fi

echo "--- same tree still loads with NO anchor (backward compatible) ---"
output="$(
  BYTECODE_KEY="$KEY" OPDUMP_MODE=load-tree OPDUMP_MAP="$OUT/bytecode.map" \
    "$PHP_BIN" -n -d extension="$SO" -f "$TREE/main.php"
)"
if [[ "$(printf '%s\n' "$output" | tail -n 1)" != "tree:loader" ]]; then
  echo "unsealed-anchor load failed (backward compat broken): $output" >&2
  exit 1
fi

echo
echo "--- tampered manifest + freshly forged manifest.sig is STILL refused by the seal ---"
# Edit the manifest, then regenerate a VALID manifest.sig with the shared key an
# attacker in this position holds -- proving the seal, not the HMAC, is what stops
# them. (Done in PHP so the test needs no crypto tooling beyond the PHP CLI.)
BYTECODE_KEY="$KEY" "$PHP_BIN" -r '
  $out = $argv[1];
  $mf = $out . "/bytecode.manifest.json";
  $mp = $out . "/bytecode.map";
  $j = file_get_contents($mf);
  $j = str_replace("\"protection_mode\": \"shared-key\"", "\"protection_mode\": \"evil-mode\"", $j);
  file_put_contents($mf, $j);
  $ikm = hex2bin(getenv("BYTECODE_KEY"));
  $sk = hash_hkdf("sha256", $ikm, 32, "bytecode-manifest-auth", "");
  $sig = hash_hmac("sha256", file_get_contents($mf) . "\0" . file_get_contents($mp), $sk);
  file_put_contents($out . "/bytecode.manifest.sig", $sig . "\n");
' "$OUT"

# Destroy the real source so a signature-check bug that silently falls back to
# normal compilation cannot accidentally produce the right answer and mask it.
printf '<?php this is not valid php syntax !!! %%%%%%\n' > "$TREE/main.php"
printf '<?php this is also not valid php syntax !!! %%%%%%\n' > "$TREE/lib.php"

set +e
output="$(
  BYTECODE_KEY="$KEY" OPDUMP_VENDOR_PUBKEY_FILE="$PUB" \
    OPDUMP_MODE=load-tree OPDUMP_MAP="$OUT/bytecode.map" \
    "$PHP_BIN" -n -d extension="$SO" -f "$TREE/main.php" 2>&1
)"
status=$?
set -e

if [[ "$status" -eq 0 ]]; then
  echo "tampered+forged-HMAC package unexpectedly loaded successfully" >&2
  echo "$output" >&2
  exit 1
fi
if ! grep -q 'digest does not match seal' <<<"$output"; then
  echo "tampered package failed without the expected seal diagnostic" >&2
  echo "$output" >&2
  exit 1
fi
if grep -q 'tree:loader' <<<"$output"; then
  echo "protected code executed despite seal failure" >&2
  echo "$output" >&2
  exit 1
fi

echo
echo "rung 10 vendor-seal: PASS"
