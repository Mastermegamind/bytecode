#!/usr/bin/env bash
# Source obfuscation workflow smoke: standalone obfuscation preserves OOP/PSR
# surface names, and obfuscate-then-encode still loads via the original source
# path map.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SO="$ROOT/php/src/modules/opdump.so"
PHP_BIN="${PHP_BIN:-php8.4}"
KEY="${BYTECODE_KEY:-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef}"

if [[ ! -f "$SO" ]]; then
  echo "opdump.so not built -- run bytecode-install-loader --build-only first" >&2
  exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
APP="$WORK/app"
OBF="$WORK/obf"
DUMP_OBF="$WORK/dump-obf"
OUT="$WORK/out"
mkdir -p "$APP/src/Controller" "$APP/templates"

cat > "$APP/src/Controller/HomeController.php" <<'PHP'
<?php
namespace App\Controller;

use PDO;

class HomeController
{
    public function index(PDO $pdo, int $id): array
    {
        $stmt = $pdo->prepare('select :id');
        $ok = $stmt !== false;
        return ['ok' => $ok, 'id' => $id];
    }
}
PHP

cat > "$APP/run.php" <<'PHP'
<?php
namespace App\Controller;

require __DIR__ . '/src/Controller/HomeController.php';

function plain_locals(int $a, int $b): int
{
    $sum = $a + $b;
    $extra = $sum * 2;
    return $extra;
}

function dynamic_sensitive(int $seed): array
{
    $local = $seed + 1;
    return compact('local');
}

echo HomeController::class, "\n";
echo plain_locals(2, 3), "\n";
echo dynamic_sensitive(5)['local'], "\n";
PHP

printf '<h1>{{ $title }}</h1>\n' > "$APP/templates/home.blade.php"

"$PHP_BIN" "$ROOT/php/bin/bytecode-obfuscate" --include-assets "$APP" "$OBF" >/dev/null
"$PHP_BIN" -l "$OBF/run.php" >/dev/null
"$PHP_BIN" -l "$OBF/src/Controller/HomeController.php" >/dev/null

grep -q 'class HomeController' "$OBF/src/Controller/HomeController.php"
grep -q 'function index(PDO $pdo, int $id)' "$OBF/src/Controller/HomeController.php"
grep -q '\$_v0 = $pdo->prepare' "$OBF/src/Controller/HomeController.php"
grep -q '\$_v1 = \$_v0 !== false' "$OBF/src/Controller/HomeController.php"
grep -q '\$local = $seed + 1' "$OBF/run.php"
test -f "$OBF/templates/home.blade.php"
grep -q '"format": "bytecode-obfuscate-v1"' "$OBF/bytecode.obfuscate.json"

"$PHP_BIN" "$ROOT/php/bin/bytecode-dump" --workflow obfuscate-only --include-assets "$APP" "$DUMP_OBF" >/dev/null
test -f "$DUMP_OBF/run.php"
test -f "$DUMP_OBF/templates/home.blade.php"
grep -q '"format": "bytecode-obfuscate-v1"' "$DUMP_OBF/bytecode.obfuscate.json"

expected="$("$PHP_BIN" -n "$APP/run.php")"
BYTECODE_KEY="$KEY" PHP_BIN="$PHP_BIN" "$PHP_BIN" "$ROOT/php/bin/bytecode-dump" \
  --workflow obfuscate-then-encode --include-assets "$APP" "$OUT" >/dev/null

if [[ -d "$OUT/.bytecode-obfuscated-input" ]]; then
  echo "temporary obfuscated input leaked into encoded output" >&2
  exit 1
fi
grep -q '"workflow": "obfuscate-then-encode"' "$OUT/bytecode.manifest.json"
grep -q '"bytecode_obfuscate": false' "$OUT/bytecode.manifest.json"

printf '<?php this source should not parse after encoding\n' > "$APP/run.php"
actual="$(BYTECODE_KEY="$KEY" OPDUMP_MODE=load-tree OPDUMP_MAP="$OUT/bytecode.map" \
  "$PHP_BIN" -n -d zend_extension="$SO" -f "$APP/run.php" 2>/dev/null)"

if [[ "$actual" != "$expected" ]]; then
  echo "unexpected obfuscate-then-encode output: $actual (expected: $expected)" >&2
  exit 1
fi

ENC_THEN="$WORK/encode_then"
BYTECODE_KEY="$KEY" PHP_BIN="$PHP_BIN" "$PHP_BIN" "$ROOT/php/bin/bytecode-dump" \
  --workflow encode-then-obfuscate "$APP/src/Controller/HomeController.php" "$ENC_THEN" >/dev/null
test -f "$ENC_THEN/bytecode.obfuscated-source/HomeController.php"

echo "source-obfuscate-workflows: PASS"
