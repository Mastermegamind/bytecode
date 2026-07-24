#!/usr/bin/env bash
# Rung 4 of the Phase 0 test ladder: try/catch/finally table round-trip.
set -euo pipefail
cd "$(dirname "$0")/../src"

SO="$(pwd)/modules/opdump.so"
PHP_BIN="${PHP_BIN:-php8.4}"
if [[ ! -f "$SO" ]]; then
  echo "opdump.so not built -- run: phpize8.4 && ./configure --with-php-config=/usr/bin/php-config8.4 && make" >&2
  exit 1
fi

BLOB="$(mktemp)"
DECOY="$(mktemp --suffix=.php)"
trap 'rm -f "$BLOB" "$DECOY"' EXIT

printf '<?php this is not valid php syntax !!! %%%%%%\n' > "$DECOY"

echo "--- dump ---"
OPDUMP_MODE=dump OPDUMP_OUT="$BLOB" "$PHP_BIN" -n -d extension="$SO" -f ../tests/rung4.php

echo
echo "--- load (pointed at invalid-syntax decoy file, to prove source text is never read) ---"
OPDUMP_MODE=load OPDUMP_IN="$BLOB" "$PHP_BIN" -n -d extension="$SO" -f "$DECOY"
echo
echo "rung 4: PASS"
