#!/usr/bin/env bash
# Build the Flutter macOS app and copy the bytecode tooling into the .app.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_NAME="${APP_NAME:-Bytecode Encoder}"
PHP_BIN="${PHP_BIN:-php}"
PHPIZE_BIN="${PHPIZE_BIN:-phpize}"
PHP_CONFIG_BIN="${PHP_CONFIG_BIN:-php-config}"
VERSION="${VERSION:-$(tr -d '[:space:]' < "$ROOT/VERSION" 2>/dev/null || printf 'v1.0.0')}"
DIST_DIR="$ROOT/dist"
APP_PATH="$ROOT/ui/build/macos/Build/Products/Release/ui.app"
OUT_APP="$DIST_DIR/$APP_NAME.app"
OUT_ZIP="$DIST_DIR/Bytecode_Encoder-macos.zip"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "macOS builds must run on macOS." >&2
  exit 2
fi

echo "== Build PHP extension =="
(
  cd "$ROOT/php/src"
  native_version="${VERSION#v}"
  make clean >/dev/null 2>&1 || true
  "$PHPIZE_BIN" >/dev/null
  CFLAGS="-DOPDUMP_VERSION=\\\"$native_version\\\"" ./configure --with-php-config="$(command -v "$PHP_CONFIG_BIN")" >/dev/null
  CFLAGS="-DOPDUMP_VERSION=\\\"$native_version\\\"" make >/dev/null
)

echo "== Build Flutter macOS app =="
(
  cd "$ROOT/ui"
  flutter build macos
)

echo "== Assemble macOS app bundle =="
rm -rf "$OUT_APP" "$OUT_ZIP"
mkdir -p "$DIST_DIR"
cp -R "$APP_PATH" "$OUT_APP"

resources="$OUT_APP/Contents/Resources"
mkdir -p \
  "$resources/bytecode/php/src/modules" \
  "$resources/php/bin"

cp -a "$ROOT/php/bin" "$resources/bytecode/php/"
cp "$ROOT/php/src/modules/opdump.so" "$resources/bytecode/php/src/modules/opdump.so"
cp "$(command -v "$PHP_BIN")" "$resources/php/bin/php"
chmod +x "$resources/php/bin/php"

echo "== Zip app bundle =="
(
  cd "$DIST_DIR"
  ditto -c -k --sequesterRsrc --keepParent "$APP_NAME.app" "$(basename "$OUT_ZIP")"
)

echo
echo "macOS app built:"
echo "$OUT_APP"
echo "$OUT_ZIP"
