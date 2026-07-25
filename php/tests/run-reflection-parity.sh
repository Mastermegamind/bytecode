#!/usr/bin/env bash
# Phase 3 groundwork: compare source-compiled reflection metadata with the
# same files reconstructed by the loader from BYTC1 containers.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/php/src"
SO="$SRC/modules/opdump.so"
PHP_BIN="${PHP_BIN:-php8.4}"
KEY="${BYTECODE_KEY:-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef}"

if [[ ! -f "$SO" ]]; then
  echo "opdump.so not built -- run php/tests/run-version-matrix.sh first" >&2
  exit 1
fi

TREE="$(mktemp -d)"
OUT="$(mktemp -d)"
EXPECTED="$(mktemp)"
ACTUAL="$(mktemp)"
trap 'rm -rf "$TREE" "$OUT"; rm -f "$EXPECTED" "$ACTUAL"' EXIT

cp "$ROOT/php/tests/reflection_parity_subject.php" "$TREE/reflection_parity_subject.php"
cp "$ROOT/php/tests/reflection_parity_probe.php" "$TREE/reflection_parity_probe.php"

"$PHP_BIN" -n -f "$TREE/reflection_parity_probe.php" > "$EXPECTED"

BYTECODE_KEY="$KEY" PHP_BIN="$PHP_BIN" \
  "$ROOT/php/bin/bytecode-dump" "$TREE/reflection_parity_subject.php" "$OUT" >/dev/null

printf '<?php this is not valid php syntax !!! %%%%%%\n' > "$TREE/reflection_parity_subject.php"

BYTECODE_KEY="$KEY" OPDUMP_MODE=load-tree OPDUMP_MAP="$OUT/bytecode.map" \
  "$PHP_BIN" -n -d extension="$SO" -f "$TREE/reflection_parity_probe.php" | tail -n 1 > "$ACTUAL"

if ! "$PHP_BIN" -n "$ROOT/php/tests/reflection_parity_diff.php" "$EXPECTED" "$ACTUAL"; then
  echo "reflection parity: FAIL" >&2
  exit 1
fi

echo "reflection parity: PASS"
