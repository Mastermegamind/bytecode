#!/usr/bin/env bash
# Preflight scanner smoke test: ensure Yakpro-inspired dynamic PHP hazards are
# detected before bytecode packaging or future symbol rewriting.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PHP_BIN="${PHP_BIN:-php8.4}"
SCANNER="$ROOT/php/bin/bytecode-scan"
SUBJECT="$ROOT/php/tests/scan_subject.php"

output="$("$PHP_BIN" "$SCANNER" "$SUBJECT")"
for code in \
  variable-variable \
  dynamic-function-call \
  dynamic-class \
  dynamic-member \
  nonliteral-symbol-name \
  dynamic-dispatch \
  callable-array \
  pdo-fetch-obj
do
  if ! grep -q "\\[$code\\]" <<<"$output"; then
    echo "missing scanner warning: $code" >&2
    echo "$output" >&2
    exit 1
  fi
done

json="$("$PHP_BIN" "$SCANNER" --json "$SUBJECT")"
if ! "$PHP_BIN" -r '
    $decoded = json_decode(stream_get_contents(STDIN), true, flags: JSON_THROW_ON_ERROR);
    if (($decoded["format"] ?? "") !== "bytecode-scan-v1") exit(1);
    if (($decoded["files_scanned"] ?? 0) !== 1) exit(1);
    if (($decoded["warning_count"] ?? 0) < 8) exit(1);
' <<<"$json"; then
  echo "scanner JSON output failed validation" >&2
  echo "$json" >&2
  exit 1
fi

if "$PHP_BIN" "$SCANNER" --fail-on-warning "$SUBJECT" >/dev/null; then
  echo "--fail-on-warning should exit nonzero when warnings are present" >&2
  exit 1
fi

echo "bytecode-scan: PASS"
