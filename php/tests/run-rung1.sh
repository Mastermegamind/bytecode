#!/usr/bin/env bash
# Rung 1 of the Phase 0 test ladder (docs/PHASE0.md): dump a trivial
# top-level script, then reload+execute it from the blob while pointing at
# a file containing deliberately invalid PHP syntax -- if output still
# matches, the loader never touched source text at all.
set -euo pipefail
cd "$(dirname "$0")/../src"

SO="$(pwd)/modules/opdump.so"
if [[ ! -f "$SO" ]]; then
  echo "opdump.so not built -- run: phpize8.4 && ./configure --with-php-config=/usr/bin/php-config8.4 && make" >&2
  exit 1
fi

BLOB="$(mktemp)"
DECOY="$(mktemp --suffix=.php)"
trap 'rm -f "$BLOB" "$DECOY"' EXIT

printf '<?php this is not valid php syntax !!! %%%%%%\n' > "$DECOY"

echo "--- dump ---"
OPDUMP_MODE=dump OPDUMP_OUT="$BLOB" php8.4 -n -d extension="$SO" -f ../tests/rung1.php

echo
echo "--- load (pointed at invalid-syntax decoy file, to prove source text is never read) ---"
OPDUMP_MODE=load OPDUMP_IN="$BLOB" php8.4 -n -d extension="$SO" -f "$DECOY"
echo
echo "rung 1: PASS"
