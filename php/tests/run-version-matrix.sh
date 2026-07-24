#!/usr/bin/env bash
# Phase 2 scaffold: build/test the extension across installed PHP minors once
# Phase 0's PHP 8.4 go/no-go is complete.
set -euo pipefail

if [[ "${PHASE0_GO:-0}" != "1" ]]; then
  echo "Phase 2 is blocked until Phase 0 rungs 3-5 pass. Set PHASE0_GO=1 after the go/no-go report." >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/php/src"

for version in 8.1 8.2 8.3 8.4 8.5; do
  phpize_bin="phpize$version"
  php_config_bin="php-config$version"
  php_bin="php$version"

  command -v "$phpize_bin" >/dev/null
  command -v "$php_config_bin" >/dev/null
  command -v "$php_bin" >/dev/null

  echo "== PHP $version =="
  (
    cd "$SRC"
    "$phpize_bin" >/dev/null
    ./configure --with-php-config="$(command -v "$php_config_bin")" >/dev/null
    make >/dev/null
  )

  PHP_BIN="$php_bin" "$ROOT/php/tests/run-rung1.sh"
  PHP_BIN="$php_bin" "$ROOT/php/tests/run-rung2.sh"
done
