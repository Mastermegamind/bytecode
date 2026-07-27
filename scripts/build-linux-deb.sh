#!/usr/bin/env bash
# Build the Flutter Linux app and package it with the bytecode tooling as a
# Debian package.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_NAME="Bytecode Encoder"
APP_ID="bytecode-encoder"
VERSION="${VERSION:-$(tr -d '[:space:]' < "$ROOT/VERSION" 2>/dev/null || printf 'v1.0.0')}"
DEB_VERSION="${VERSION#v}"
ARCH="${DEB_ARCH:-amd64}"
PHP_VERSION="${PHP_VERSION:-8.4}"
BUILD_DIR="$ROOT/build/deb"
PKGROOT="$BUILD_DIR/${APP_ID}_${DEB_VERSION}_${ARCH}"
DIST_DIR="$ROOT/dist"

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

echo "== Assemble Debian package root =="
rm -rf "$PKGROOT"
mkdir -p \
  "$PKGROOT/DEBIAN" \
  "$PKGROOT/opt/$APP_ID/app" \
  "$PKGROOT/opt/$APP_ID/php/bin" \
  "$PKGROOT/opt/$APP_ID/bytecode/php/src/modules" \
  "$PKGROOT/usr/bin" \
  "$PKGROOT/usr/share/applications" \
  "$PKGROOT/usr/share/icons/hicolor/scalable/apps" \
  "$DIST_DIR"

cp -a "$ROOT/ui/build/linux/x64/release/bundle/." "$PKGROOT/opt/$APP_ID/app/"
cp "$(command -v "$php_bin")" "$PKGROOT/opt/$APP_ID/php/bin/php"
cp -a "$ROOT/php/bin" "$PKGROOT/opt/$APP_ID/bytecode/php/"
cp "$ROOT/php/src/modules/opdump.so" "$PKGROOT/opt/$APP_ID/bytecode/php/src/modules/opdump.so"

cat > "$PKGROOT/usr/bin/$APP_ID" <<EOF_LAUNCHER
#!/usr/bin/env bash
set -euo pipefail

export BYTECODE_ROOT="/opt/$APP_ID/bytecode"
export BYTECODE_PHP="/opt/$APP_ID/php/bin/php"
export PATH="/opt/$APP_ID/php/bin:\$PATH"

exec "/opt/$APP_ID/app/ui" "\$@"
EOF_LAUNCHER
chmod +x "$PKGROOT/usr/bin/$APP_ID" "$PKGROOT/opt/$APP_ID/php/bin/php"

cat > "$PKGROOT/usr/share/applications/$APP_ID.desktop" <<EOF_DESKTOP
[Desktop Entry]
Type=Application
Name=$APP_NAME
Exec=$APP_ID
Icon=$APP_ID
Categories=Development;Utility;
Terminal=false
EOF_DESKTOP

cat > "$PKGROOT/usr/share/icons/hicolor/scalable/apps/$APP_ID.svg" <<'EOF_SVG'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128">
  <rect width="128" height="128" rx="20" fill="#1f2937"/>
  <path fill="#0f766e" d="M24 35h80v58H24z"/>
  <path fill="#fef7ed" d="M36 49h22v8H36zm0 16h40v8H36zm0 16h30v8H36z"/>
  <path fill="#f59e0b" d="M82 48l18 16-18 16-6-7 10-9-10-9z"/>
</svg>
EOF_SVG

installed_size="$(
  du -sk "$PKGROOT" | awk '{print $1}'
)"

cat > "$PKGROOT/DEBIAN/control" <<EOF_CONTROL
Package: $APP_ID
Version: $DEB_VERSION
Section: devel
Priority: optional
Architecture: $ARCH
Maintainer: MegaMind Technologies LTD <hello@megamindtechnologies.com>
Installed-Size: $installed_size
Depends: libc6, libgtk-3-0, libstdc++6
Description: Bytecode Encoder desktop app
 GUI wrapper for bytecode-dump and bytecode-verify with bundled PHP tooling.
EOF_CONTROL

find "$PKGROOT/DEBIAN" -type d -exec chmod 0755 {} +
find "$PKGROOT/DEBIAN" -type f -exec chmod 0644 {} +
chmod u-s,g-s,o-t "$PKGROOT/DEBIAN"

echo "== Build .deb =="
dpkg-deb --build --root-owner-group "$PKGROOT" "$DIST_DIR/${APP_ID}_${DEB_VERSION}_${ARCH}.deb"

echo
echo "Debian package built:"
echo "$DIST_DIR/${APP_ID}_${DEB_VERSION}_${ARCH}.deb"
