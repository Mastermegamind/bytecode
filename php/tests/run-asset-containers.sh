#!/usr/bin/env bash
# Mixed package smoke: PHP files use Zend bytecode containers, while
# html/css/js/twig/blade files use encrypted raw asset containers.
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
OUT_DEFAULT="$(mktemp -d)"
trap 'rm -rf "$TREE" "$OUT" "$OUT_DEFAULT"' EXIT

cat > "$TREE/index.php" <<'PHP'
<?php
echo "mixed:php\n";
PHP

mkdir -p "$TREE/assets" "$TREE/templates"
printf '<!doctype html><title>Bytecode</title><h1>encoded html</h1>' > "$TREE/page.html"
printf 'body { color: #123456; }\n' > "$TREE/assets/app.css"
printf 'console.log("encoded js");\n' > "$TREE/assets/app.js"
printf '<p>{{ name }}</p>\n' > "$TREE/templates/view.twig"
printf '<h1>{{ $name }}</h1>\n' > "$TREE/templates/home.blade.php"

BYTECODE_KEY="$KEY" PHP_BIN="$PHP_BIN" "$ROOT/php/bin/bytecode-dump" "$TREE" "$OUT_DEFAULT" >/dev/null
if [[ -e "$OUT_DEFAULT/assets/app.css" || -e "$OUT_DEFAULT/assets/app.js" || -e "$OUT_DEFAULT/page.html" || -e "$OUT_DEFAULT/templates/view.twig" || -e "$OUT_DEFAULT/templates/home.blade.php" ]]; then
  echo "assets were encoded without --include-assets" >&2
  exit 1
fi

BYTECODE_KEY="$KEY" PHP_BIN="$PHP_BIN" "$ROOT/php/bin/bytecode-dump" --include-assets "$TREE" "$OUT" >/dev/null
BYTECODE_KEY="$KEY" "$PHP_BIN" "$ROOT/php/bin/bytecode-verify" "$OUT/bytecode.manifest.json" >/dev/null

printf '<?php this is not valid php syntax !!! %%%%%%\n' > "$TREE/index.php"
php_output="$(
  BYTECODE_KEY="$KEY" OPDUMP_MODE=load-tree OPDUMP_MAP="$OUT/bytecode.map" \
    "$PHP_BIN" -n -d extension="$SO" -f "$TREE/index.php"
)"
if [[ "$(printf '%s\n' "$php_output" | tail -n 1)" != "mixed:php" ]]; then
  echo "unexpected PHP output: $php_output" >&2
  exit 1
fi

for rel in page.html assets/app.css assets/app.js templates/view.twig templates/home.blade.php; do
  decoded="$(
    BYTECODE_KEY="$KEY" "$PHP_BIN" -r \
      'require $argv[1]; echo bytecode_asset_decrypt($argv[2]);' \
      "$ROOT/php/runtime/bytecode-assets.php" "$OUT/$rel"
  )"
  if [[ "$decoded" != "$(cat "$TREE/$rel")" ]]; then
    echo "asset decode mismatch for $rel" >&2
    exit 1
  fi
done

echo "asset containers: PASS"
