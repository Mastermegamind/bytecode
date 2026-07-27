# bytecode

**[Read the full How-To guide →](HOWTO.md)** — CLI and GUI walkthroughs with
tested, copy-pasteable examples for every feature below.

`bytecode` is an open implementation of a PHP bytecode encoder and loader. It
compiles PHP source through the real Zend Engine compiler, serializes the
resulting runtime bytecode into encrypted containers, and reloads those
containers later without reading the original PHP source.

The project is in the same broad category as commercial PHP encoders such as
ionCube, SourceGuardian, and the old Zend Guard, but the design goal here is to
make the moving parts visible: the Zend loader, the container format, the CLI
tooling, the verification tests, and the desktop packaging are all developed in
the open.

The long-term shape is language-agnostic: one container/tooling model with
backend adapters per runtime. Today, the only backend is PHP, because Zend
bytecode round-tripping is the proven path in this repository.

## Current Status

The PHP backend currently builds and passes the local version matrix for:

- PHP 8.1
- PHP 8.2
- PHP 8.3
- PHP 8.4
- PHP 8.5

The matrix builds the native extension against each minor version's Zend
headers and runs the current dump/load, Reflection parity, scanner, tree-load,
and container tests.

```bash
PHP_VERSIONS='8.1 8.2 8.3 8.4 8.5' php/tests/run-version-matrix.sh
```

See [`HOWTO.md`](HOWTO.md) for usage (CLI and GUI), and [`docs/PLAN.md`](docs/PLAN.md)
for the phased roadmap and the detailed phase notes:

- [`HOWTO.md`](HOWTO.md): how to use the CLI tools and the desktop GUI, with examples
- [`docs/PHASE0.md`](docs/PHASE0.md): Zend dump/load feasibility spike
- [`docs/PHASE1.md`](docs/PHASE1.md): encrypted container and CLI packer
- [`docs/PHASE2.md`](docs/PHASE2.md): PHP version matrix
- [`docs/PHASE3.md`](docs/PHASE3.md): Reflection parity
- [`docs/PHASE4.md`](docs/PHASE4.md): packaging, keys, licensing, integrity
- [`docs/PHASE4_KEY_MODEL.md`](docs/PHASE4_KEY_MODEL.md): implemented key model
- [`docs/ZEND_INTERNALS.md`](docs/ZEND_INTERNALS.md): Zend porting notes
- [`docs/YAKPRO_PO_STUDY.md`](docs/YAKPRO_PO_STUDY.md): lessons from Yakpro PO

## Why This Exists

PHP source obfuscation is useful for slowing casual reading, but it is not the
same as bytecode encoding. Obfuscators still emit valid PHP source text. A
bytecode encoder changes the trust boundary: source is compiled once, converted
to runtime structures, packed into authenticated containers, and loaded by a
native extension later.

That matters for commercial PHP distribution, private SaaS/customer
deployments, and any environment where application code needs to run on a
customer-controlled machine without shipping the original source tree in plain
text.

This project also exists because PHP encoders are easy to get subtly wrong.
Reflection, dependency injection, named arguments, default values, class
metadata, typed properties, method dispatch, and runtime caches all depend on
Zend internals matching the version that is executing the code. A bytecode
loader that "mostly works" can still break real applications. The test ladder
here is built around catching those failures.

## Architecture

The repository has four main layers.

### 1. Native Zend Extension

The core loader is in [`php/src/opdump.c`](php/src/opdump.c). It hooks
`zend_compile_file` and supports several runtime modes through environment
variables:

- `OPDUMP_MODE=dump`: compile PHP normally, then serialize the generated
  `zend_op_array` to a blob.
- `OPDUMP_MODE=load`: read one blob and return a reconstructed `zend_op_array`
  instead of parsing the source file.
- `OPDUMP_MODE=load-tree`: intercept requested source paths and load matching
  encoded containers through a generated `bytecode.map`.

The extension reconstructs opcodes, literals, functions, classes, properties,
argument metadata, try/catch metadata, runtime caches, and enough class/function
state for the current test ladder. Because Zend internals change between PHP
minor versions, the loader contains compatibility branches for 8.1 through 8.5.

### 2. Container Format

The CLI tools wrap raw Zend blobs and protected assets into encrypted
`BYTC2`-format containers. The output file keeps the source's own filename and
extension (e.g. `index.php` in → `index.php` out, encrypted) so it's a drop-in
replacement — it's identified by content, not by a special extension. (`--raw`
debug output is the exception, written as `<name>.opd2`.) PHP files are stored
as Zend opcode dumps (`php-zend-opdump`/`OPD2`). When `bytecode-dump` is run
with `--include-assets`, `.html`, `.htm`, `.css`, `.js`, `.mjs`, `.twig`, and
`.blade.php` files are stored as encrypted raw assets (`bytecode-asset`/`RAW1`)
for serving through `php/runtime/bytecode-assets.php`.

`BYTC2` provides:

- AES-256-GCM authenticated encryption for each container payload.
- HKDF-SHA256 key derivation from `BYTECODE_KEY` or `OPDUMP_KEY`, so the
  environment value is input keying material rather than the raw AES key. For
  normal local encoding, `bytecode-dump` defaults to `build/vendor-secret.key`
  and auto-creates that file when it is missing.
- A manifest signature over `bytecode.manifest.json` and `bytecode.map`, so a
  deployment cannot silently rewrite the source-to-container map.
- Optional license mode where a per-build data encryption key is RSA-wrapped
  for the runtime holder of the private key.

`BYTC1`, the earlier shared-secret format, remains a supported read path for
compatibility.

### 3. PHP CLI Tooling

The command-line tools under [`php/bin`](php/bin) provide the user-facing
workflow:

- `bytecode-keygen`: generate a random shared key, or regenerate the default
  `build/vendor-secret.key` with `--vendor-secret --force`.
- `bytecode-dump`: walk files/directories, dump bytecode, pack encoded
  containers, write the manifest/map, support dry-runs, profiles, scans, and
  optional asset containers.
- `bytecode-pack`: pack one raw dump into a container.
- `bytecode-pack-asset`: pack one HTML/CSS/JS/Twig asset into an encrypted
  raw-asset container.
- `bytecode-doctor`: preflight PHP, loader, key, SAPI, and toolchain setup.
- `bytecode-info`: inspect a container header and metadata.
- `bytecode-verify`: verify manifests, hashes, sizes, map entries, and
  signatures; `--decrypt-test` also proves the configured key decrypts every
  container.
- `bytecode-key-rotate`: regenerate the default vendor secret and re-encode
  source-backed builds.
- `bytecode-deploy`: sync an app to a remote staging path, encode there,
  verify, start a staged runner, cut over with backups, or roll back.
- `bytecode-selftest`: run the local smoke/regression suite.
- `bytecode-package-sign`: sign a whole encoded output directory with a bundle
  manifest.
- `bytecode-env-pack`: encrypt `.env` or config files as asset containers.
- `bytecode-laravel`: prepare Laravel caches before encoding.
- `bytecode-license-keygen`: generate RSA license keys.
- `bytecode-vendor-keygen`: generate the vendor Ed25519 signing keypair used to
  seal packages (tamper-proof licensing); prints the compile-in public-key hex.
  Seals can also carry signed machine policy (`--machine-id`, hostnames,
  fingerprints, expiry, activation token), letting vendor-secret/shared-key
  containers run only when the loader has the vendor public-key anchor and the
  runtime machine matches.
- `bytecode-scan`: preflight source scanner for dynamic PHP hazards.

Twig templates can be protected two ways. For normal Twig rendering, compile the
Twig cache to PHP and encode that cache as PHP bytecode. If you include raw
`.twig` files in a dump with `--include-assets`, they are protected as encrypted
assets and can be read through `php/runtime/BytecodeTwigLoader.php`. Laravel
Blade templates (`.blade.php`) are protected the same way and can be compiled
from encrypted source through `php/runtime/BytecodeBladeCompiler.php`. Static
assets can be served through `php/runtime/BytecodeAssetMiddleware.php`, and
encrypted config/env files can be loaded with `php/runtime/BytecodeConfig.php`.

The scanner was inspired by Yakpro PO's practical PHP-obfuscation lessons. It
does not replace bytecode encoding; it warns about constructs that affect safe
name handling, bytecode compatibility, and future symbol rewriting, such as
dynamic calls, variable variables, callable arrays, dynamic class/member access,
enums, attributes, readonly classes, trait adaptations, `eval()`, Fibers, and
framework-sensitive object hydration patterns.

### 4. Flutter Desktop UI And Packages

The [`ui`](ui) directory is a Flutter desktop application intended to wrap the
CLI workflow. The packaging scripts can bundle the UI, PHP runtime, helper
scripts, and native extension into installable desktop artifacts.

Supported packaging paths include:

- Linux AppImage
- Linux `.deb`
- macOS `.app` zip
- Windows zip
- Windows MSI

Packaging workflows live in [`scripts`](scripts) and
[`.github/workflows`](.github/workflows).

## Repository Layout

```text
php/src/                      Native Zend extension
php/bin/                      Encoder, verifier, scanner, key, and license tools
php/tests/                    Rung tests, parity tests, matrix runner
docs/                         Roadmap, phase docs, internals notes
ui/                           Flutter desktop application
scripts/                      Linux/macOS/Windows packaging scripts
.github/workflows/            CI for PHP matrix and desktop packages
```

## Versioning

The project version lives in [`VERSION`](VERSION). Use `bytecode-version` to
read or set it:

```bash
php8.4 php/bin/bytecode-version
php8.4 php/bin/bytecode-version --set v0.0.1phase2
```

That value is embedded into new loader builds, `bytecode.manifest.json`,
`bytecode.package.json`, `bytecode-doctor --json`, and the GUI package-version
default.

## Build The PHP Extension

Build against a specific PHP minor version by pairing the matching `phpize` and
`php-config` binaries:

```bash
cd php/src
phpize8.4
./configure --enable-opdump --with-php-config=/usr/bin/php-config8.4
make
cd ../..
```

Then run a smoke test:

```bash
PHP_BIN=php8.4 php/tests/run-rung1.sh
```

The easiest CLI path is the installer helper. It builds the native loader for
the requested PHP minor, copies it into that version's extension directory,
writes a Debian-style `mods-available` ini file when available, and enables it
with `phpenmod`:

```bash
php8.4 php/bin/bytecode-install-loader --php-version 8.4
```

Enable additional SAPIs:

```bash
php php/bin/bytecode-install-loader --php-version 8.4 --sapi cli,fpm,apache2
```

Only build and print the test command:

```bash
php php/bin/bytecode-install-loader --php-version 8.4 --build-only
```

The Flutter UI exposes the same helper in **Build Setup**. Set the target PHP
version, then use the construction button to build the loader or the install
button to install it as a Zend extension. If the desktop process cannot prompt
for sudo, run the printed `bytecode-install-loader` command in a terminal.

For ionCube-style loading and a `php -v` banner entry, load the same shared
object as a Zend extension:

```bash
php8.4 -n -d zend_extension=/var/www/bytecode/php/src/modules/opdump.so -v
```

Expected shape:

```text
Zend Engine v4.4.23, Copyright (c) Zend Technologies
    with Bytecode PHP Loader v0.0.1phase2, Copyright (c) 2026 MegaMind Technologies LTD, by MegaMind Technologies LTD
```

Runtime bytecode loading works through the Zend-extension path too:

```bash
BYTECODE_KEY="$KEY" \
OPDUMP_MODE=load-tree \
OPDUMP_MAP="$OUT/bytecode.map" \
php8.4 \
  -d zend_extension=/var/www/bytecode/php/src/modules/opdump.so \
  -f /path/to/app/public/index.php
```

On macOS:

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

## Test Ladder

The tests are intentionally incremental. Each rung proves a little more Zend
state survives dump/load:

- `run-rung1.sh`: top-level script op_array
- `run-rung2.sh`: function definition, typed parameter, default value
- `run-rung3.sh`: class, constructor, typed property, method call
- `run-rung4.sh`: try/catch/finally metadata
- `run-rung5-reflection.sh`: Reflection spot check
- `run-reflection-parity.sh`: source-vs-loader Reflection snapshots
- `run-load-tree.sh`: map-driven loading of encoded source paths
- `run-phase1-container.sh`: encrypted container load path
- `run-rung6-hkdf.sh`: HKDF key derivation
- `run-rung7-license.sh`: RSA-wrapped license key mode
- `run-rung8-manifest-auth.sh`: manifest/map authentication
- `run-rung9-obfuscate.sh`: safe local-variable obfuscation
- `run-bytecode-scan.sh`: preflight scanner warnings
- `run-bytecode-config-scan.sh`: `bytecode.json` scanner/dumper integration

Run the current suite for one PHP version:

```bash
PHP_VERSIONS=8.4 php/tests/run-version-matrix.sh
```

Run every installed supported version:

```bash
PHP_VERSIONS='8.1 8.2 8.3 8.4 8.5' php/tests/run-version-matrix.sh
```

## Encode A PHP Project

Generate a key, choose an output directory, and encode first-party PHP files
into a separate tree. Do not write encoded artifacts back into the application
source directory.

```bash
cd /var/www/bytecode

KEY="$(php/bin/bytecode-keygen)"
OUT="/tmp/app-bytecode"

BYTECODE_KEY="$KEY" php/bin/bytecode-dump \
  --exclude '.history/*' \
  --exclude 'vendor/*' \
  --exclude 'storage/*' \
  --exclude 'node_modules/*' \
  /path/to/app \
  "$OUT"
```

Verify the encoded output:

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
  -f /path/to/app/public/index.php
```

For Composer, PHP-DI, Slim, RoadRunner, Laravel-like, or framework-heavy
applications, start conservatively:

- Encode first-party PHP files first.
- Leave `vendor/`, `.env`, `storage/`, caches, public assets, generated files,
  and editor history outside the encoded set.
- Keep framework and Composer internals running from normal source until the
  parity suite covers the application shape.
- Use `bytecode-scan` before encoding to find dynamic constructs.

## Project Configuration

`bytecode-dump` and `bytecode-scan` can read a project-level `bytecode.json`.
It centralizes excludes and scanner behavior:

```json
{
  "exclude": ["vendor/*", "storage/*", "node_modules/*"],
  "scanner": {
    "enabled": true,
    "fail_on_warning": false,
    "ignore_codes": []
  },
  "symbols": {
    "classes": "preserve",
    "methods": "preserve",
    "properties": "preserve",
    "functions": "preserve",
    "variables": "internal"
  }
}
```

Use it directly:

```bash
php/bin/bytecode-scan --json --config bytecode.json app

BYTECODE_KEY="$KEY" php/bin/bytecode-dump \
  --config bytecode.json \
  app \
  /tmp/app-bytecode
```

When scanning is enabled, diagnostics are written into
`bytecode.manifest.json`.

## Key And License Modes

### Shared Key Mode

Shared key mode is the simplest development and deployment model:

```bash
KEY="$(php/bin/bytecode-keygen)"

BYTECODE_KEY="$KEY" php/bin/bytecode-dump app /tmp/bytecode-out

BYTECODE_KEY="$KEY" \
OPDUMP_MODE=load-tree \
OPDUMP_MAP=/tmp/bytecode-out/bytecode.map \
php8.4 -n -d extension=php/src/modules/opdump.so -f app/index.php
```

The same key material must be present at encode time and runtime. Wrong keys
fail during AES-GCM authentication before payload parsing.

### License Key Mode

License mode avoids handing the runtime a raw shared build key. The encoder
generates one random data encryption key per build, wraps it with a public RSA
key, and writes `bytecode.license.json`. Runtime unpacks that key with the
private RSA key.

```bash
php/bin/bytecode-license-keygen /path/to/keys

BYTECODE_LICENSE_PUBKEY=/path/to/keys/license.pub.pem \
  php/bin/bytecode-dump app /tmp/bytecode-out

OPDUMP_LICENSE_KEY_FILE=/path/to/keys/license.key.pem \
OPDUMP_MODE=load-tree \
OPDUMP_MAP=/tmp/bytecode-out/bytecode.map \
php8.4 -n -d extension=php/src/modules/opdump.so -f app/index.php
```

## Obfuscation

The primary protection model is encrypted bytecode containers, but the CLI and
GUI also support a conservative source obfuscation stage. Choose the workflow
that matches the build you want:

```bash
php8.4 php/bin/bytecode-dump --workflow encode-only app /tmp/bytecode-out
php8.4 php/bin/bytecode-dump --workflow obfuscate-only app /tmp/obfuscated-src
php8.4 php/bin/bytecode-dump --workflow obfuscate-then-encode app /tmp/bytecode-out
php8.4 php/bin/bytecode-dump --workflow encode-then-obfuscate app /tmp/bytecode-out
```

The source obfuscator preserves namespaces, class names, function names, method
names, properties, constants, parameters, superglobals, globals, and framework
entry points. It only renames safe function/method local variables, so OOP,
PSR-style autoloading, MVC controllers, and PDO type hints keep their public
shape. Bodies using `compact()`, `extract()`, `get_defined_vars()`, `global`,
`static`, nested closures, or variable variables are skipped rather than
rewritten.

The older `--obfuscate` flag is still available as an extra bytecode-level
local-variable metadata pass during encoding. It is separate from
`--workflow`; use it only when you want that additional compiled-bytecode
rename layer.

Class names, method names, property names, function names, and public symbols
are currently preserved. More aggressive symbol rewriting needs application
metadata and Reflection parity coverage before it is safe.

## Desktop Packages

Build the Flutter desktop UI, PHP extension, PHP helper scripts, and runtime
pieces into one Linux AppImage:

```bash
scripts/build-linux-appimage.sh
```

Output:

```bash
dist/Bytecode_Encoder-x86_64.AppImage
```

Build a Debian package:

```bash
scripts/build-linux-deb.sh
```

Output:

```bash
dist/bytecode-encoder_1.0.0_amd64.deb
```

Override package metadata:

```bash
VERSION=1.2.0 DEB_ARCH=amd64 PHP_VERSION=8.4 scripts/build-linux-deb.sh
```

Build a macOS `.app` bundle and zip on macOS:

```bash
scripts/build-macos-app.sh
```

Output:

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

MSI output:

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

For a UI-only Windows smoke package:

```powershell
.\scripts\build-windows-package.ps1 -SkipExtension
```

GitHub Actions builds desktop artifacts in
[`.github/workflows/desktop-packages.yml`](.github/workflows/desktop-packages.yml).
Linux and macOS include the native PHP extension. The default Windows package
workflow can produce UI-only artifacts until a native Windows extension build is
provided.

## Continuous Integration

The PHP version matrix workflow is in
[`.github/workflows/php-version-matrix.yml`](.github/workflows/php-version-matrix.yml).
It runs the matrix against PHP 8.1 through 8.5.

Desktop packaging CI is in
[`.github/workflows/desktop-packages.yml`](.github/workflows/desktop-packages.yml).

## What Is Not Finished Yet

This is still a rapidly evolving encoder, not a hardened commercial product.
Important remaining work includes:

- Broader real-application fixture coverage.
- Deeper Reflection parity for more PHP language features.
- OPcache behavior decisions.
- Native Windows extension build automation.
- Stronger Flutter UI workflow around dumping, verifying, and inspecting
  manifests.
- More complete support for advanced PHP features such as enums, readonly
  edge cases, intersection/union metadata, attributes, closures, generators,
  and framework-specific dynamic dispatch patterns.
- Production policy around license expiry, revocation, and deployment
  rotation.

## License

Apache License 2.0 - see [`LICENSE`](LICENSE). Copyright remains with
MegaMind Technologies LTD and contributors. See [`NOTICE`](NOTICE) for
attribution and trademark notes.

Official website: https://megamindtechnologies.com
Contact: hello@megamindtechnologies.com
