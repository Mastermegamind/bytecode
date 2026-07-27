#!/usr/bin/env bash
# Cross-file inheritance with child + parent instance properties. This used to
# segfault in zend_do_link_class() because restored property_info entries had a
# NULL prototype pointer when Zend rebuilt the inherited properties table.
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

cat > "$TREE/P.php" <<'PHP'
<?php
class P
{
    public string $p = 'pm';

    public function m(): string
    {
        return $this->p;
    }
}
PHP

cat > "$TREE/C.php" <<'PHP'
<?php
class C extends P
{
    public int $x = 1;

    public function go(): string
    {
        return $this->x . $this->m();
    }
}
PHP

cat > "$TREE/autoload.php" <<'PHP'
<?php
spl_autoload_register(function (string $class): void {
    $file = __DIR__ . '/' . str_replace('\\', '_', $class) . '.php';
    if (is_file($file)) {
        require $file;
    }
});
PHP

cat > "$TREE/run.php" <<'PHP'
<?php
require __DIR__ . '/autoload.php';
$c = new C();
echo $c->go(), "\n";
PHP

BYTECODE_KEY="$KEY" PHP_BIN="$PHP_BIN" "$ROOT/php/bin/bytecode-dump" "$TREE" "$OUT" >/dev/null

for file in "$TREE"/*.php; do
  printf '<?php broken !!!\n' > "$file"
done

output="$(
  BYTECODE_KEY="$KEY" OPDUMP_MODE=load-tree OPDUMP_MAP="$OUT/bytecode.map" \
    "$PHP_BIN" -n -d zend_extension="$SO" "$TREE/run.php"
)"

if [[ "$(printf '%s\n' "$output" | tail -n 1)" != "1pm" ]]; then
  echo "unexpected output: $output" >&2
  exit 1
fi

echo "inheritance props: PASS"
