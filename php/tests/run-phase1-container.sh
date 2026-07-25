#!/usr/bin/env bash
# Phase 1 smoke: dump encrypted BYTC1 containers and load one without parsing
# source text.
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
trap 'rm -f "$DECOY"; rm -rf "$OUT"' EXIT

printf '<?php this is not valid php syntax !!! %%%%%%\n' > "$DECOY"

BYTECODE_KEY="$KEY" PHP_BIN="$PHP_BIN" "$ROOT/php/bin/bytecode-dump" "$ROOT/php/tests" "$OUT"

for rung in rung1 rung2 rung3; do
  echo "--- load $rung BYTC1 (pointed at invalid-syntax decoy file) ---"
  BYTECODE_KEY="$KEY" OPDUMP_MODE=load OPDUMP_IN="$OUT/$rung.php" "$PHP_BIN" -n -d extension="$SO" -f "$DECOY"
  echo
done

echo "phase 1 container: PASS"
