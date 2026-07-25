#!/usr/bin/env bash
# bytecode.json config smoke: excludes and scanner options apply both to
# bytecode-scan directly and to bytecode-dump --scan manifest diagnostics.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/php/src"
SO="$SRC/modules/opdump.so"
PHP_BIN="${PHP_BIN:-php8.4}"
KEY="${BYTECODE_KEY:-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef}"

WORK="$(mktemp -d)"
OUT="$(mktemp -d)"
trap 'rm -rf "$WORK" "$OUT"' EXIT

mkdir -p "$WORK/app" "$WORK/vendor"
cp "$ROOT/php/tests/scan_subject.php" "$WORK/app/scan_subject.php"
cp "$ROOT/php/tests/scan_subject.php" "$WORK/vendor/ignored.php"
cp "$ROOT/php/tests/rung1.php" "$WORK/app/rung1.php"

cat > "$WORK/bytecode.json" <<'JSON'
{
  "exclude": ["vendor/*"],
  "scanner": {
    "enabled": true,
    "ignore_codes": ["pdo-fetch-obj"],
    "fail_on_warning": false
  },
  "symbols": {
    "classes": "preserve",
    "methods": "preserve",
    "properties": "preserve",
    "functions": "preserve",
    "variables": "internal"
  }
}
JSON

json="$("$PHP_BIN" "$ROOT/php/bin/bytecode-scan" --json --config "$WORK/bytecode.json" "$WORK")"
"$PHP_BIN" -r '
    $decoded = json_decode(stream_get_contents(STDIN), true, flags: JSON_THROW_ON_ERROR);
    if (($decoded["files_scanned"] ?? 0) !== 2) exit(1);
    foreach ($decoded["warnings"] as $warning) {
        if (str_contains($warning["file"], "/vendor/")) exit(2);
        if (($warning["code"] ?? "") === "pdo-fetch-obj") exit(3);
    }
' <<<"$json"

if [[ ! -f "$SO" ]]; then
  echo "opdump.so not built; skipping bytecode-dump manifest scan integration"
  echo "bytecode config scan: PASS"
  exit 0
fi

BYTECODE_KEY="$KEY" PHP_BIN="$PHP_BIN" \
  "$ROOT/php/bin/bytecode-dump" --config "$WORK/bytecode.json" "$WORK" "$OUT" >/dev/null

"$PHP_BIN" -r '
    $manifest = json_decode(file_get_contents($argv[1]), true, flags: JSON_THROW_ON_ERROR);
    if (($manifest["scan"]["format"] ?? "") !== "bytecode-scan-v1") exit(1);
    if (($manifest["scan"]["files_scanned"] ?? 0) !== 2) exit(2);
    foreach ($manifest["files"] as $file) {
        if (str_starts_with($file["source"], "vendor/")) exit(3);
    }
    foreach ($manifest["scan"]["warnings"] as $warning) {
        if (str_contains($warning["file"], "/vendor/")) exit(4);
        if (($warning["code"] ?? "") === "pdo-fetch-obj") exit(5);
    }
' "$OUT/bytecode.manifest.json"

echo "bytecode config scan: PASS"
