#!/usr/bin/env bash
# Rung 8: manifest/map authentication. Each BYTC2 container's own GCM tag
# only authenticates that container's own bytes -- nothing previously
# stopped bytecode.map from being edited to point one source path at a
# *different*, still validly-encrypted container; each file would still
# decrypt and authenticate fine on its own. bytecode-dump now writes
# bytecode.manifest.sig (an HMAC-SHA256 over the manifest+map under an
# HKDF-derived signing key); load-tree mode verifies it before trusting any
# map entry. Confirms an untampered tree still loads, and that swapping two
# map entries' output columns is detected and refused -- with source text
# destroyed first, so there's no way a fallback to real compilation could
# accidentally produce the right answer and mask a broken check.
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
trap 'rm -rf "$TREE" "$OUT"' EXIT

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

BYTECODE_KEY="$KEY" PHP_BIN="$PHP_BIN" "$ROOT/php/bin/bytecode-dump" "$TREE" "$OUT" >/dev/null

if [[ ! -f "$OUT/bytecode.manifest.sig" ]]; then
  echo "expected bytecode.manifest.sig next to bytecode.manifest.json" >&2
  exit 1
fi

echo "--- untampered map still loads ---"
output="$(
  BYTECODE_KEY="$KEY" OPDUMP_MODE=load-tree OPDUMP_MAP="$OUT/bytecode.map" \
    "$PHP_BIN" -n -d extension="$SO" -f "$TREE/main.php"
)"
if [[ "$(printf '%s\n' "$output" | tail -n 1)" != "tree:loader" ]]; then
  echo "unexpected output: $output" >&2
  exit 1
fi

echo
echo "--- tampered map (swapped output columns) is refused before any file loads ---"
awk -F'\t' '{print $1"\t"($1 ~ /main\.php$/ ? "lib.php" : "main.php")}' "$OUT/bytecode.map" > "$OUT/bytecode.map.tampered"
mv "$OUT/bytecode.map.tampered" "$OUT/bytecode.map"

# Destroy the real source too, so a signature-check bug that silently falls
# back to normal compilation can't accidentally produce the right answer.
printf '<?php this is not valid php syntax !!! %%%%%%\n' > "$TREE/main.php"
printf '<?php this is also not valid php syntax !!! %%%%%%\n' > "$TREE/lib.php"

set +e
output="$(
  BYTECODE_KEY="$KEY" OPDUMP_MODE=load-tree OPDUMP_MAP="$OUT/bytecode.map" \
    "$PHP_BIN" -n -d extension="$SO" -f "$TREE/main.php" 2>&1
)"
status=$?
set -e

if [[ "$status" -eq 0 ]]; then
  echo "tampered map unexpectedly loaded successfully" >&2
  echo "$output" >&2
  exit 1
fi
if ! grep -q 'bytecode.manifest.sig mismatch' <<<"$output"; then
  echo "tampered map failed without the expected manifest.sig diagnostic" >&2
  echo "$output" >&2
  exit 1
fi

echo
echo "rung 8 manifest-auth: PASS"
