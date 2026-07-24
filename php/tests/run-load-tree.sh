#!/usr/bin/env bash
# Recursive tree loader smoke: main.php requires lib.php, then both source files
# are replaced with invalid syntax before execution.
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

printf '<?php this is not valid php syntax !!! %%%%%%\n' > "$TREE/main.php"
printf '<?php this is also not valid php syntax !!! %%%%%%\n' > "$TREE/lib.php"

output="$(
  BYTECODE_KEY="$KEY" OPDUMP_MODE=load-tree OPDUMP_MAP="$OUT/bytecode.map" \
    "$PHP_BIN" -n -d extension="$SO" -f "$TREE/main.php"
)"

if [[ "$(printf '%s\n' "$output" | tail -n 1)" != "tree:loader" ]]; then
  echo "unexpected output: $output" >&2
  exit 1
fi

echo "load-tree: PASS"
