#!/usr/bin/env bash
# Rung 5 of the Phase 0 test ladder: Reflection parity for PHP-DI-like
# constructor autowiring metadata.
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

cp "$ROOT/php/tests/rung5_subject.php" "$TREE/rung5_subject.php"
cp "$ROOT/php/tests/rung5_probe.php" "$TREE/rung5_probe.php"

expected="$("$PHP_BIN" -n -f "$TREE/rung5_probe.php")"

BYTECODE_KEY="$KEY" PHP_BIN="$PHP_BIN" "$ROOT/php/bin/bytecode-dump" "$TREE" "$OUT" >/dev/null

printf '<?php this is not valid php syntax !!! %%%%%%\n' > "$TREE/rung5_subject.php"
printf '<?php this is also not valid php syntax !!! %%%%%%\n' > "$TREE/rung5_probe.php"

actual="$(
  BYTECODE_KEY="$KEY" OPDUMP_MODE=load-tree OPDUMP_MAP="$OUT/bytecode.map" \
    "$PHP_BIN" -n -d extension="$SO" -f "$TREE/rung5_probe.php" | tail -n 1
)"

if [[ "$actual" != "$expected" ]]; then
  echo "reflection mismatch" >&2
  echo "expected: $expected" >&2
  echo "actual:   $actual" >&2
  exit 1
fi

echo "rung 5 reflection: PASS"
