#!/usr/bin/env bash
# Build the Flutter Linux app and package it with the PHP bytecode tooling as
# a single AppImage.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_NAME="Bytecode Encoder"
APP_ID="bytecode-encoder"
VERSION="${VERSION:-$(tr -d '[:space:]' < "$ROOT/VERSION" 2>/dev/null || printf 'v1.0.0')}"
ARCH="${ARCH:-x86_64}"
PHP_VERSION="${PHP_VERSION:-8.4}"
BUILD_DIR="$ROOT/build/appimage"
APPDIR="$BUILD_DIR/AppDir"
DIST_DIR="$ROOT/dist"
LINUXDEPLOY="$BUILD_DIR/linuxdeploy-$ARCH.AppImage"

php_bin="php$PHP_VERSION"
phpize_bin="phpize$PHP_VERSION"
php_config_bin="php-config$PHP_VERSION"

if ! command -v "$php_bin" >/dev/null; then
  php_bin="php"
fi
if ! command -v "$phpize_bin" >/dev/null; then
  phpize_bin="phpize"
fi
if ! command -v "$php_config_bin" >/dev/null; then
  php_config_bin="php-config"
fi

echo "== Build PHP extension with $php_bin =="
(
  cd "$ROOT/php/src"
  native_version="${VERSION#v}"
  make clean >/dev/null 2>&1 || true
  "$phpize_bin" >/dev/null
  CFLAGS="-DOPDUMP_VERSION=\\\"$native_version\\\"" ./configure --with-php-config="$(command -v "$php_config_bin")" >/dev/null
  CFLAGS="-DOPDUMP_VERSION=\\\"$native_version\\\"" make >/dev/null
)

echo "== Build Flutter Linux bundle =="
(
  cd "$ROOT/ui"
  flutter build linux
)

echo "== Assemble AppDir =="
rm -rf "$APPDIR" "$DIST_DIR"
mkdir -p \
  "$APPDIR/usr/bin/$APP_ID" \
  "$APPDIR/usr/lib/bytecode/php/src/modules" \
  "$APPDIR/usr/share/applications" \
  "$APPDIR/usr/share/icons/hicolor/scalable/apps" \
  "$DIST_DIR"

cp -a "$ROOT/ui/build/linux/x64/release/bundle/." "$APPDIR/usr/bin/$APP_ID/"
cp "$(command -v "$php_bin")" "$APPDIR/usr/bin/php"
cp -a "$ROOT/php/bin" "$APPDIR/usr/lib/bytecode/php/"
cp "$ROOT/php/src/modules/opdump.so" "$APPDIR/usr/lib/bytecode/php/src/modules/opdump.so"

cat > "$APPDIR/AppRun" <<'SH'
#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export APPDIR="$HERE"
export PATH="$APPDIR/usr/bin:$PATH"

exec "$APPDIR/usr/bin/bytecode-encoder/ui" "$@"
SH
chmod +x "$APPDIR/AppRun" "$APPDIR/usr/bin/php"

cat > "$APPDIR/usr/share/applications/$APP_ID.desktop" <<EOF_DESKTOP
[Desktop Entry]
Type=Application
Name=$APP_NAME
Exec=$APP_ID
Icon=$APP_ID
Categories=Development;Utility;
Terminal=false
EOF_DESKTOP
cp "$APPDIR/usr/share/applications/$APP_ID.desktop" "$APPDIR/$APP_ID.desktop"

cat > "$APPDIR/usr/share/icons/hicolor/scalable/apps/$APP_ID.svg" <<'EOF_SVG'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128">
  <rect width="128" height="128" rx="20" fill="#1f2937"/>
  <path fill="#0f766e" d="M24 35h80v58H24z"/>
  <path fill="#fef7ed" d="M36 49h22v8H36zm0 16h40v8H36zm0 16h30v8H36z"/>
  <path fill="#f59e0b" d="M82 48l18 16-18 16-6-7 10-9-10-9z"/>
</svg>
EOF_SVG
cp "$APPDIR/usr/share/icons/hicolor/scalable/apps/$APP_ID.svg" "$APPDIR/$APP_ID.svg"

if [[ ! -x "$LINUXDEPLOY" ]]; then
  echo "== Download linuxdeploy =="
  mkdir -p "$BUILD_DIR"
  curl -L \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage" \
    -o "$LINUXDEPLOY"
  chmod +x "$LINUXDEPLOY"
fi

echo "== Build AppImage =="
(
  cd "$ROOT"
  APPIMAGE_EXTRACT_AND_RUN=1 \
  OUTPUT="$DIST_DIR/Bytecode_Encoder-${VERSION#v}-$ARCH.AppImage" \
    "$LINUXDEPLOY" \
      --appdir "$APPDIR" \
      --executable "$APPDIR/usr/bin/$APP_ID/ui" \
      --executable "$APPDIR/usr/bin/php" \
      --desktop-file "$APPDIR/usr/share/applications/$APP_ID.desktop" \
      --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/$APP_ID.svg" \
      --output appimage
)

echo
echo "AppImage built:"
echo "$DIST_DIR/Bytecode_Encoder-${VERSION#v}-$ARCH.AppImage"
