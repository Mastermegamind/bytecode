# bytecode

An engine for compiling source languages down to their runtime's native
bytecode, dumping that bytecode to an encoded container, and reloading it
back into the runtime without the original source — the same category of
tool as ionCube/SourceGuardian/Zend Guard, built in the open.

The long-term goal is language-agnostic: a common container format and
tooling shape that different per-language backends plug into. For now there
is exactly one backend, because it's the only one that's been proven to
work at all:

## Status: PHP backend, Phase 0 (feasibility spike)

Targeting PHP **8.1, 8.2, 8.3, 8.4, 8.5**. See [`docs/PLAN.md`](docs/PLAN.md)
for the full phased roadmap, [`docs/PHASE0.md`](docs/PHASE0.md) for the
current feasibility gate, and [`docs/PHASE1.md`](docs/PHASE1.md),
[`docs/PHASE2.md`](docs/PHASE2.md), [`docs/PHASE3.md`](docs/PHASE3.md), and
[`docs/PHASE4.md`](docs/PHASE4.md) for the implementation track and gated
follow-up phases.

Phase 0 is a hard go/no-go gate: a minimal Zend extension that dumps a
compiled `zend_op_array` to disk and reloads it in a fresh process, tested
against PHP 8.4 only, including a Reflection-parity check (the exact class of
bug — `ReflectionParameter::isDefaultValueAvailable()` silently wrong — that
made an existing commercial encoder unusable for a real PHP-DI-based
application). If that doesn't hold up cleanly, the multi-version work in
later phases isn't worth starting.

## Why this exists

PHP source obfuscation (renaming identifiers, escaping strings) is not real
protection — the output is still valid, fully-readable PHP source text.
Actual bytecode encoding requires hooking the Zend Engine's compile step
directly, which is what this project builds toward, version by version.

## Current tools

```bash
php/tests/run-rung1.sh
php/tests/run-rung2.sh
php/tests/run-rung3.sh
php/tests/run-rung4.sh
php/tests/run-rung5-reflection.sh
php/tests/run-load-tree.sh
php/tests/run-phase1-container.sh
php/tests/run-rung6-hkdf.sh
php/tests/run-rung7-license.sh
php/tests/run-rung8-manifest-auth.sh
php/tests/run-rung9-obfuscate.sh
php/tests/run-bytecode-scan.sh

php/bin/bytecode-keygen
php/bin/bytecode-scan php/tests
php/bin/bytecode-info /tmp/bytecode-out/rung1.php.bytc
php/bin/bytecode-verify /tmp/bytecode-out/bytecode.manifest.json

BYTECODE_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef \
  php/bin/bytecode-dump php/tests /tmp/bytecode-out

BYTECODE_KEY=... php/bin/bytecode-dump app routes/config.php /tmp/bytecode-out

BYTECODE_KEY=... OPDUMP_MODE=load-tree OPDUMP_MAP=/tmp/bytecode-out/bytecode.map \
  php8.4 -n -d extension=php/src/modules/opdump.so -f path/to/entrypoint.php
```

Containers default to `BYTC2`: `BYTECODE_KEY`/`OPDUMP_KEY` is treated as HKDF
input keying material (not the raw AES key), and `bytecode.manifest.json` +
`bytecode.map` are authenticated together via `bytecode.manifest.sig`, closing
the gap where the map could be edited to point a source path at a different,
still-validly-encrypted container. `BYTC1` (the original shared-secret format)
remains a supported read path. See
[`docs/PHASE4_KEY_MODEL.md`](docs/PHASE4_KEY_MODEL.md) for the full model.

Optional license mode replaces the shared secret with an RSA-wrapped
per-build key, so the vendor encoding a build never has to hand the customer
running it a raw shared key:

```bash
php/bin/bytecode-license-keygen /path/to/keys   # writes license.key.pem + license.pub.pem

BYTECODE_LICENSE_PUBKEY=/path/to/keys/license.pub.pem \
  php/bin/bytecode-dump app /tmp/bytecode-out   # writes bytecode.license.json too

OPDUMP_LICENSE_KEY_FILE=/path/to/keys/license.key.pem \
  OPDUMP_MODE=load-tree OPDUMP_MAP=/tmp/bytecode-out/bytecode.map \
  php8.4 -n -d extension=php/src/modules/opdump.so -f path/to/entrypoint.php
```

Optional variable-name obfuscation renames non-parameter locals to `_v0`,
`_v1`, ... in the dumped bytecode, skipped per-function whenever
`compact()`/`extract()`/`get_defined_vars()`/`$$name` could depend on the
real name (safe because compiled opcodes reference locals by slot index, not
name — see `opdump_op_array_uses_dynamic_vars()` in `php/src/opdump.c`):

```bash
BYTECODE_KEY=... php/bin/bytecode-dump --obfuscate app /tmp/bytecode-out
```

macOS build shape:

```bash
brew install php openssl@3 pkg-config
cd php/src
phpize
./configure --enable-opdump --with-php-config="$(brew --prefix php)/bin/php-config"
make
cd ../..
php/tests/run-rung1.sh
```

`config.m4` checks `pkg-config openssl` first, then Homebrew `openssl@3`, then
falls back to the Linux-style `-lcrypto` link.

## Desktop Packages

Build the Flutter desktop UI, the PHP extension, and the PHP helper scripts
into one AppImage:

```bash
scripts/build-linux-appimage.sh
```

The output is written to:

```bash
dist/Bytecode_Encoder-x86_64.AppImage
```

The AppImage contains the Flutter bundle under `usr/bin/bytecode-encoder`, a
bundled PHP CLI at `usr/bin/php`, and the bytecode tooling/extension under
`usr/lib/bytecode`. When launched from an AppImage, the GUI automatically uses
that embedded bytecode root.

To build against a different installed PHP minor:

```bash
PHP_VERSION=8.3 scripts/build-linux-appimage.sh
```

Build a Debian package:

```bash
scripts/build-linux-deb.sh
```

The output is written to:

```bash
dist/bytecode-encoder_1.0.0_amd64.deb
```

Override package metadata when needed:

```bash
VERSION=1.2.0 DEB_ARCH=amd64 PHP_VERSION=8.4 scripts/build-linux-deb.sh
```

Build a macOS `.app` bundle and zip on macOS:

```bash
scripts/build-macos-app.sh
```

The output is written to:

```bash
dist/Bytecode_Encoder-macos.zip
```

Build a Windows zip package on Windows:

```powershell
.\scripts\build-windows-package.ps1
```

Build a Windows zip and MSI installer with WiX Toolset:

```powershell
dotnet tool install --global wix
.\scripts\build-windows-package.ps1 -BuildMsi
```

The MSI output is written to:

```powershell
dist\Bytecode_Encoder-windows-x64.msi
```

For a complete Windows package, provide a PHP for Windows directory and a
native `php_opdump.dll` built with the PHP SDK:

```powershell
$env:PHP_DIR = "C:\php-8.4"
$env:OPDUMP_DLL = "C:\path\to\php_opdump.dll"
.\scripts\build-windows-package.ps1 -BuildMsi
```

If you only want to smoke-test the Flutter Windows package:

```powershell
.\scripts\build-windows-package.ps1 -SkipExtension
```

GitHub Actions builds desktop artifacts in
`.github/workflows/desktop-packages.yml`. Linux and macOS include the native
PHP extension in the package. The default Windows CI job is UI-only until a
Windows `php_opdump.dll` build step is added, but it still produces both the
zip package and MSI installer.

## Using It On A Large PHP Project

Encode the project into a separate output directory. Do not write encoded
artifacts back into the application tree. You can pass one file/folder or many
files/folders; when more than one input is passed, the output keeps the selected
paths under their common parent directory.

```bash
cd /var/www/bytecode

KEY="$(php/bin/bytecode-keygen)"
OUT="/tmp/exam-bytecode"

BYTECODE_KEY="$KEY" php/bin/bytecode-dump \
  --exclude '.history/*' \
  --exclude 'vendor/*' \
  --exclude 'storage/*' \
  --exclude 'node_modules/*' \
  /var/www/exam.test \
  "$OUT"
```

Verify the manifest, container headers, hashes, sizes, map file, and (when
`BYTECODE_KEY`/`OPDUMP_KEY` is set) the `bytecode.manifest.sig` signature:

```bash
php/bin/bytecode-verify "$OUT/bytecode.manifest.json"
```

Run an entrypoint through the encoded tree:

```bash
BYTECODE_KEY="$KEY" \
OPDUMP_MODE=load-tree \
OPDUMP_MAP="$OUT/bytecode.map" \
php8.4 \
  -d extension=/var/www/bytecode/php/src/modules/opdump.so \
  -f /var/www/exam.test/public/index.php
```

For Composer/PHP-DI/Slim/RoadRunner apps, start by encoding first-party PHP
files and leaving `vendor/`, `.env`, `storage/`, public assets, caches, and
editor history out of the encoded set. Composer, PHP-DI, and framework code can
then keep running normally while matching application files are loaded from
`.bytc` containers through `bytecode.map`.

Keep the key safe and stable for the deployment. The same `BYTECODE_KEY` used
to pack the files is required at runtime; the C loader rejects wrong keys
during AES-GCM authentication before executing the payload.

## License

MIT — see [`LICENSE`](LICENSE).
