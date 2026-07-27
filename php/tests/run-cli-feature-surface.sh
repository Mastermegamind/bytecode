#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PHP_BIN="${PHP_BIN:-php8.4}"
TMP="$(mktemp -d)"
KEY_FILE="$ROOT/build/vendor-secret.key"
KEY_BACKUP="$TMP/vendor-secret.key.bak"
if [[ -f "$KEY_FILE" ]]; then
  cp "$KEY_FILE" "$KEY_BACKUP"
fi
restore_key() {
  if [[ -f "$KEY_BACKUP" ]]; then
    cp "$KEY_BACKUP" "$KEY_FILE"
  fi
  rm -rf "$TMP"
}
trap restore_key EXIT

mkdir -p "$TMP/src"
VERSION="$("$PHP_BIN" "$ROOT/php/bin/bytecode-version")"
printf '<?php echo "ok";\n' > "$TMP/src/a.php"
printf 'body{}\n' > "$TMP/src/a.css"
printf '<h1>{{ $name }}</h1>\n' > "$TMP/src/a.blade.php"
printf '<?php #[Attr] readonly class A {} enum E { case A; } eval("echo 1;"); new class {}; Fiber::suspend();\n' > "$TMP/src/scan.php"

printf '%s\n' "$VERSION" | grep -q '^v'
"$PHP_BIN" "$ROOT/php/bin/bytecode-version" --json | grep -q '"format": "bytecode-version-v1"'

dry="$("$PHP_BIN" "$ROOT/php/bin/bytecode-dump" --dry-run --report-json --profile php-assets "$TMP/src" "$TMP/out")"
grep -q '"format": "bytecode-dry-run-v1"' <<<"$dry"
grep -q '"planned_assets": 2' <<<"$dry"
for profile in php-only php-assets laravel slim symfony codeigniter wordpress-plugin wordpress-theme yii cakephp full-app; do
  "$PHP_BIN" "$ROOT/php/bin/bytecode-dump" --dry-run --profile "$profile" "$TMP/src" "$TMP/out-$profile" >/dev/null
done

"$PHP_BIN" "$ROOT/php/bin/bytecode-scan" "$TMP/src/scan.php" > "$TMP/scan.out"
for code in enum attribute readonly-class anonymous-class eval fiber; do
  grep -q "\\[$code\\]" "$TMP/scan.out"
done

"$PHP_BIN" "$ROOT/php/bin/bytecode-key-rotate" --vendor-secret-only --force >/dev/null
test -f "$ROOT/build/vendor-secret.key"

"$PHP_BIN" "$ROOT/php/bin/bytecode-dump" --include-assets "$TMP/src/a.php" "$TMP/src/a.css" "$TMP/src/a.blade.php" "$TMP/encoded" >/dev/null
grep -q "\"tool_version\": \"$VERSION\"" "$TMP/encoded/bytecode.manifest.json"
"$PHP_BIN" "$ROOT/php/bin/bytecode-verify" --decrypt-test "$TMP/encoded" | grep -q 'decrypt-test: PASS'
"$PHP_BIN" "$ROOT/php/bin/bytecode-package-sign" "$TMP/encoded" >/dev/null
test -f "$TMP/encoded/bytecode.package.json"

printf 'APP_SECRET=abc\n' > "$TMP/.env"
"$PHP_BIN" "$ROOT/php/bin/bytecode-env-pack" "$TMP/.env" "$TMP/.env.bytc"
test "$(head -c4 "$TMP/.env.bytc")" = "BYTC"

"$PHP_BIN" "$ROOT/php/bin/bytecode-doctor" --json >/dev/null || true

echo "cli-feature-surface: PASS"
