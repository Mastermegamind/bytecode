#!/usr/bin/env bash
# Build/test the extension across PHP minors. CI sets PHP_VERSIONS to one
# version per job; local runs can keep the full default list.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/php/src"
PHP_VERSIONS="${PHP_VERSIONS:-8.1 8.2 8.3 8.4}"
TEST_SCRIPTS=(
  "$ROOT/php/tests/run-rung1.sh"
  "$ROOT/php/tests/run-rung2.sh"
  "$ROOT/php/tests/run-rung3.sh"
  "$ROOT/php/tests/run-rung4.sh"
  "$ROOT/php/tests/run-rung5-reflection.sh"
  "$ROOT/php/tests/run-reflection-parity.sh"
  "$ROOT/php/tests/run-load-tree.sh"
  "$ROOT/php/tests/run-phase1-container.sh"
)

for version in $PHP_VERSIONS; do
  phpize_bin="phpize$version"
  php_config_bin="php-config$version"
  php_bin="php$version"

  if ! command -v "$phpize_bin" >/dev/null; then
    phpize_bin="phpize"
  fi
  if ! command -v "$php_config_bin" >/dev/null; then
    php_config_bin="php-config"
  fi
  if ! command -v "$php_bin" >/dev/null; then
    php_bin="php"
  fi

  echo "== PHP $version =="
  (
    cd "$SRC"
    make clean >/dev/null 2>&1 || true
    "$phpize_bin" >/dev/null
    ./configure --with-php-config="$(command -v "$php_config_bin")" >/dev/null
    make >/dev/null
  )

  actual_version="$("$php_bin" -r 'echo PHP_MAJOR_VERSION.".".PHP_MINOR_VERSION;')"
  if [[ "$actual_version" != "$version" ]]; then
    echo "Expected PHP $version but $php_bin reports $actual_version" >&2
    exit 1
  fi

  for script in "${TEST_SCRIPTS[@]}"; do
    PHP_BIN="$php_bin" "$script"
  done
done
