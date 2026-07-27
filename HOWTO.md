# How To Use Bytecode Encoder

A practical, example-driven guide to encoding PHP source into encrypted
`BYTC2` containers and running it, using either the command-line tools or
the desktop GUI. For the architecture and roadmap, see
[`README.md`](README.md) and [`docs/PLAN.md`](docs/PLAN.md). For the
container/key-model internals, see
[`docs/PHASE4_KEY_MODEL.md`](docs/PHASE4_KEY_MODEL.md).

Every command in this guide was run against this repository while writing
it — none of it is hypothetical.

## Table of Contents

1. [Concepts](#1-concepts)
2. [Build the extension](#2-build-the-extension)
3. [CLI walkthrough](#3-cli-walkthrough)
   - [3.0 Set the project version](#30-set-the-project-version)
   - [3.1 Generate a key](#31-generate-a-key)
   - [3.2 Scan source for risky dynamic constructs](#32-scan-source-for-risky-dynamic-constructs)
   - [3.3 Encode a project](#33-encode-a-project)
   - [3.4 Verify a build](#34-verify-a-build)
   - [3.5 Inspect a single container](#35-inspect-a-single-container)
   - [3.6 Run the encoded tree](#36-run-the-encoded-tree)
   - [3.7 License mode (RSA-wrapped keys)](#37-license-mode-rsa-wrapped-keys)
   - [3.8 Variable-name obfuscation](#38-variable-name-obfuscation)
   - [3.9 The `bytecode.json` config file](#39-the-bytecodejson-config-file)
   - [3.10 Installing the loader system-wide](#310-installing-the-loader-system-wide)
4. [GUI walkthrough](#4-gui-walkthrough)
5. [Troubleshooting](#5-troubleshooting)
6. [Reference](#6-reference)

## 1. Concepts

| Term | Meaning |
|---|---|
| **`opdump`** | The Zend extension (`php/src/opdump.c`) that hooks PHP's compile step. In `dump` mode it serializes a compiled `zend_op_array` to disk instead of executing it; in `load`/`load-tree` mode it reconstructs one from disk *without ever parsing PHP source text*. |
| **Container** (`BYTC2`) | One encoded file: an AES-256-GCM-encrypted blob. PHP files hold compiled Zend bytecode (`php-zend-opdump`/`OPD2`); when `--include-assets` is enabled, `.html`, `.htm`, `.css`, `.js`, `.mjs`, `.twig`, and `.blade.php` files hold encrypted raw asset bytes (`bytecode-asset`/`RAW1`). **The output file keeps the source's own extension** (`index.php` in → `index.php` out, encrypted) — it's identified by content (the `BYTC` magic bytes), not by a special file extension, so it can be a drop-in replacement for the original file. (`--raw` debug output is the one exception: it's written as `<name>.opd2`, since it's never meant to be deployed.) |
| **`bytecode.manifest.json`** | Written once per `bytecode-dump` run. Lists every encoded file, its hash, size, and (if `--scan` was used) scan warnings. |
| **`bytecode.map`** | Tab-separated `absolute-source-path → relative-container-path` file. This is what the loader reads at runtime to know which container to load instead of a given source request. |
| **`bytecode.manifest.sig`** | HMAC-SHA256 over the manifest + map together, so editing the map to repoint a source path at a different container is detected and refused. |
| **`bytecode.license.json`** | Only written in license mode: an RSA-OAEP-SHA256-wrapped copy of the build's data-encryption key (DEK). |
| **Vendor-secret key file** | Default shared key material at `build/vendor-secret.key`. Encoding auto-creates it when missing. Regenerate deliberately with `php/bin/bytecode-keygen --vendor-secret --force`. |
| **Shared-secret mode** | You can still provide `BYTECODE_KEY`/`OPDUMP_KEY`, but normal encoding defaults to the vendor-secret key file. |
| **License mode** | An RSA keypair replaces the shared secret: you (the vendor) encode with the customer's public key; they run with their own private key. You never see or transmit their private key, and they never see a raw shared secret. |

## 2. Build the extension

Every CLI/GUI operation that encodes or runs anything needs `php/src/modules/opdump.so` built first.

```bash
cd php/src
phpize8.4
./configure --enable-opdump --with-php-config=/usr/bin/php-config8.4
make
cd ../..
```

Confirm it built:

```bash
$ ls php/src/modules/opdump.so
php/src/modules/opdump.so
```

Two optional `configure` flags bake vendor material into your custom loader:

- `--with-opdump-vendor-secret=<64hex>` — a fallback shared IKM, so encoded
  files run without a server-side `BYTECODE_KEY`.
- `--with-opdump-vendor-pubkey=<64hex>` — your Ed25519 seal trust anchor (from
  `bytecode-vendor-keygen`). With it compiled in, the loader **requires** a
  valid `bytecode.seal.json` on every package and fails closed otherwise. See
  §3.7.

macOS and other PHP minors: see [`README.md`](README.md#current-tools) for
the Homebrew build shape and per-minor `phpize`/`php-config` binaries.

## 3. CLI walkthrough

All CLI tools live in `php/bin/` and are plain PHP scripts (`#!/usr/bin/env php`) — run them with a PHP CLI binary, e.g. `php8.4 php/bin/bytecode-dump ...`. Every tool supports `-h`/`--help`.

We'll use this example project throughout:

```bash
mkdir -p /tmp/demo/app
cat > /tmp/demo/app/index.php <<'PHP'
<?php
function greet(string $name = 'world'): string
{
    return "Hello, {$name}!";
}
echo greet('Bytecode'), PHP_EOL;
PHP
```

### 3.0 Set the project version

The current release label lives in `VERSION`. Set it once, then new encoded
manifests, signed package metadata, doctor JSON, GUI package defaults, and new
loader builds will use the same label:

```bash
php8.4 php/bin/bytecode-version --set v0.0.1phase2
php8.4 php/bin/bytecode-version
```

The native loader strips only the leading `v` while compiling, so PHP still
prints the normal banner shape: `with Bytecode PHP Loader v0.0.1phase2`.

### 3.1 Generate a key

```bash
$ php8.4 php/bin/bytecode-keygen
ddf43b124e9af142fe5423b26e99bff980d380c1d7612989cd7e5cf80fed082e
```

That's 32 random bytes as 64 hex characters. Save it somewhere real for a
real project — anyone who has it can decrypt every container built with it:

```bash
KEY="$(php8.4 php/bin/bytecode-keygen)"
```

### 3.2 Scan source for risky dynamic constructs

Before encoding, `bytecode-scan` flags PHP constructs that read/write things
by a *runtime-computed name* — variable variables, `call_user_func()`,
`class_exists()` with a non-literal argument, etc. This doesn't block
encoding (the encoder handles all of it correctly), it's a heads-up for
things that make *future* symbol-level tooling (like `--obfuscate`) more
conservative, or that are just worth knowing about in a codebase you're
about to lock down.

```bash
$ php8.4 php/bin/bytecode-scan /tmp/demo/app
bytecode-scan: scanned 1 file(s), 0 warning(s)
```

With something to flag:

```bash
$ cat > /tmp/demo/app/risky.php <<'PHP'
<?php
$name = 'greet';
$$name = 'value';
call_user_func('strtoupper', 'x');
PHP

$ php8.4 php/bin/bytecode-scan /tmp/demo/app
/tmp/demo/app/risky.php:3 [variable-variable] variable variables depend on runtime names and block safe variable rewriting
/tmp/demo/app/risky.php:4 [dynamic-dispatch] call_user_func() uses runtime callable names; symbol rewriting needs an explicit policy
bytecode-scan: scanned 2 file(s), 2 warning(s)
```

Machine-readable output for CI:

```bash
$ php8.4 php/bin/bytecode-scan --json /tmp/demo/app
{
    "format": "bytecode-scan-v1",
    "files_scanned": 2,
    "warning_count": 2,
    "warnings": [
        {
            "file": "/tmp/demo/app/risky.php",
            "line": 3,
            "code": "variable-variable",
            "message": "variable variables depend on runtime names and block safe variable rewriting"
        },
        {
            "file": "/tmp/demo/app/risky.php",
            "line": 4,
            "code": "dynamic-dispatch",
            "message": "call_user_func() uses runtime callable names; symbol rewriting needs an explicit policy"
        }
    ]
}
```

Fail CI on any warning:

```bash
php8.4 php/bin/bytecode-scan --fail-on-warning /tmp/demo/app   # exit 1 if any warnings
```

Exclude paths (comma-free — repeat the flag):

```bash
php8.4 php/bin/bytecode-scan --exclude 'vendor/*' --exclude 'storage/*' /tmp/demo/app
```

Warning codes you can see: `variable-variable`, `dynamic-class`,
`dynamic-member`, `dynamic-function-call`, `callable-array`,
`pdo-fetch-obj`, `nonliteral-symbol-name`, `dynamic-dispatch`, `enum`,
`attribute`, `readonly-class`, `anonymous-class`, `trait-adaptation`, `eval`,
and `fiber`.

### 3.3 Encode a project

`bytecode-dump` walks a file or directory tree and writes one encoded
container per supported PHP file, keeping the source's own filename and
extension, so it's a drop-in replacement. Pass `--include-assets` to also
encode `.html`, `.htm`, `.css`, `.js`, `.mjs`, `.twig`, and `.blade.php` files.
PHP files are true Zend bytecode containers and are listed in `bytecode.map`;
asset files are encrypted raw containers and are listed in
`bytecode.manifest.json`.

```bash
$ BYTECODE_KEY="$KEY" PHP_BIN=php8.4 php8.4 php/bin/bytecode-dump --include-assets /tmp/demo/app /tmp/demo/out
/tmp/demo/app/index.php -> /tmp/demo/out/index.php
/tmp/demo/app/risky.php -> /tmp/demo/out/risky.php

$ ls /tmp/demo/out
bytecode.manifest.json  bytecode.manifest.sig  bytecode.map  index.php  risky.php
```

`/tmp/demo/out/index.php` is now the encrypted container, not PHP source —
`file /tmp/demo/out/index.php` or `bytecode-info` (§3.5) shows its real
contents.

`PHP_BIN` picks which `php` binary compiles the source (default `php8.4`).
For normal encrypted builds, `bytecode-dump` uses `build/vendor-secret.key` by
default and creates it automatically if it is missing. Set
`BYTECODE_VENDOR_KEY_FILE` or pass `--vendor-key-file <path>` to use a
different key file. `BYTECODE_KEY`/`OPDUMP_KEY` can still override this, and
[license mode](#37-license-mode-rsa-wrapped-keys) replaces shared key material
entirely.

To rotate the default vendor secret:

```bash
php8.4 php/bin/bytecode-keygen --vendor-secret --force
```

Or rotate and immediately re-encode from source:

```bash
php8.4 php/bin/bytecode-key-rotate \
  --source /tmp/demo/app \
  --out /tmp/demo/out-rotated \
  --include-assets
```

Preflight a build without writing containers:

```bash
php8.4 php/bin/bytecode-dump --dry-run --report-json --profile php-assets /tmp/demo/app /tmp/demo/out
```

Built-in profiles are `php-only`, `php-assets`, `laravel`, `slim`, and
`full-app`. Profiles apply include/exclude defaults; explicit flags still win.

Check the local PHP/loader/key/SAPI/toolchain setup:

```bash
php8.4 php/bin/bytecode-doctor --php-version 8.4
```

Run the local smoke suite:

```bash
php8.4 php/bin/bytecode-selftest
```

Verify that the configured key can actually decrypt every container:

```bash
php8.4 php/bin/bytecode-verify --decrypt-test /tmp/demo/out
```

Sign the full output directory as a release bundle:

```bash
php8.4 php/bin/bytecode-package-sign /tmp/demo/out
```

Encrypt a `.env` or config file and load it at runtime with
`php/runtime/BytecodeConfig.php`:

```bash
php8.4 php/bin/bytecode-env-pack /tmp/demo/app/.env /tmp/demo/out/.env.bytc
```

Stage a remote app, encode on the remote machine, and verify the generated
manifest before any cutover:

```bash
php8.4 php/bin/bytecode-deploy trigger@192.168.8.42:/var/www/exam.test \
  --local-root /var/www/exam.test \
  --stage /var/www/exam.test.stage \
  --profile slim \
  --include-assets \
  --start-runner \
  --verify-routes /,/login,/backend/login
```

After app-level verification, add `--cutover` to replace the remote app from
the staged copy. Cutover creates a timestamped backup first. Restore the latest
backup with:

```bash
php8.4 php/bin/bytecode-deploy trigger@192.168.8.42:/var/www/exam.test \
  --stage /var/www/exam.test.stage \
  --rollback
```

For Laravel apps, prepare framework caches before encoding:

```bash
php8.4 php/bin/bytecode-laravel prepare /var/www/my-laravel-app
```

To add Composer script shortcuts to an app, merge
`php/composer-scripts.example.json` into that app's `composer.json`.

**Always encode into a separate output directory, never back into the
source tree.** Since the encoded output keeps the exact same filename as its
source, pointing `<output-dir>` at (or inside) your source tree would
overwrite your `.php` files in place with their own encrypted, unrecoverable
replacements — there is no `--dry-run` and no confirmation prompt.

Multiple files/folders, excludes, and `--scan` in one pass:

```bash
BYTECODE_KEY="$KEY" php8.4 php/bin/bytecode-dump \
  --scan --fail-on-scan-warning \
  --exclude 'vendor/*' \
  --exclude 'storage/*' \
  --exclude '.git/*' \
  app routes/config.php \
  /tmp/demo/out
```

Scan results (when `--scan` is used) are embedded in the manifest under a
`"scan"` key, so a later audit doesn't need to re-scan:

```bash
$ php8.4 -r '$m = json_decode(file_get_contents($argv[1]), true); echo json_encode($m["scan"], JSON_PRETTY_PRINT);' /tmp/demo/out/bytecode.manifest.json
{
    "format": "bytecode-scan-v1",
    "files_scanned": 2,
    "warning_count": 2,
    "warnings": [ ... ]
}
```

`--raw` skips encryption entirely and writes plaintext `<name>.opd2` blobs
instead (the one case with a distinct suffix, since it's never meant to be
deployed) — useful for debugging the loader itself, never for anything you
intend to ship (no manifest/map/sig are written in `--raw` mode).

### 3.4 Verify a build

Checks container headers, manifest hashes/sizes, `bytecode.map` consistency,
and (when `BYTECODE_KEY`/`OPDUMP_KEY` is set) the manifest signature
cryptographically:

```bash
$ php8.4 php/bin/bytecode-verify /tmp/demo/out/bytecode.manifest.json
verified 2 file(s) metadata/hash/map consistency

$ BYTECODE_KEY="$KEY" php8.4 php/bin/bytecode-verify /tmp/demo/out/bytecode.manifest.json
verified 2 file(s) metadata/hash/map consistency
```

Without the key it's a structural check only (hashes, sizes, map
consistency, and that `bytecode.manifest.sig` looks like 64 hex chars). With
the key it also recomputes and checks the HMAC — if that fails, either the
key is wrong or the manifest/map was tampered with:

```bash
$ BYTECODE_KEY="0000000000000000000000000000000000000000000000000000000000000000" \
    php8.4 php/bin/bytecode-verify /tmp/demo/out/bytecode.manifest.json
/tmp/demo/out/bytecode.manifest.sig: signature mismatch (wrong key, or manifest/map tampered)
```

You can also point it directly at one container file (no manifest needed):

```bash
php8.4 php/bin/bytecode-verify /tmp/demo/out/index.php
```

### 3.5 Inspect a single container

Prints header fields without decrypting the payload — no key required:

```bash
$ php8.4 php/bin/bytecode-info /tmp/demo/out/index.php
path: /tmp/demo/out/index.php
container: BYTC2
container_version: 2
php_version_id: 80423
backend: php-zend-opdump
backend_format: OPD2
cipher_id: 1
key_id: 8a6af3dd2c70c03d29ea02867d2bf9a2
header_size: 63
nonce_size: 12
tag_size: 16
ciphertext_size: 795
size: 902
sha256: 8d03e93cbab2fb9ea1322d263175a520d9238852dd0aa9dcb1d3faddce311420
```

Useful for confirming which PHP version a build targets, or spot-checking a
container's size/hash without needing the key.

### 3.5.1 Serve encoded assets

Browser assets cannot execute as Zend bytecode. They must be decrypted and
streamed by PHP so the browser receives normal CSS/JS/HTML bytes:

```php
<?php
require __DIR__ . '/php/runtime/bytecode-assets.php';

bytecode_asset_serve(__DIR__ . '/encoded/assets/app.css');
```

Use your web server rewrite/router to send protected asset URLs to a PHP entry
point like this. The helper uses the same `BYTECODE_KEY`/`OPDUMP_KEY`/
`BYTECODE_VENDOR_KEY` material as the loader, and can also unwrap
`bytecode.license.json` when `OPDUMP_LICENSE_KEY_FILE` is set.

Twig templates protected with `--include-assets` can be read through
`php/runtime/BytecodeTwigLoader.php`.

Laravel Blade templates (`resources/views/**/*.blade.php`) are also asset
containers, not Zend bytecode. Register the Blade compiler bridge in an app
service provider after Laravel has booted its filesystem and view services:

```php
<?php
require_once '/var/www/bytecode/php/runtime/BytecodeBladeCompiler.php';

$this->app->singleton('blade.compiler', function ($app) {
    return new BytecodeBladeCompiler(
        $app['files'],
        $app['config']['view.compiled']
    );
});
```

Then encode with:

```bash
php8.4 /var/www/bytecode/php/bin/bytecode-dump \
  --profile laravel \
  --include-assets \
  /var/www/app /var/www/app-encoded
```

### 3.6 Run the encoded tree

The loader intercepts `zend_compile_file` for the whole request. Point
`OPDUMP_MAP` at the `bytecode.map` from your build, set `OPDUMP_MODE=load-tree`,
and load `opdump.so` — every `require`/`include`/entrypoint whose *source*
path appears in the map is transparently served from its encoded container
instead of being compiled from the `.php` file on disk. The container lives
at a different path (your output directory), even though it has the same
filename as the source it replaces:

```bash
$ BYTECODE_KEY="$KEY" OPDUMP_MODE=load-tree OPDUMP_MAP=/tmp/demo/out/bytecode.map \
    php8.4 -n -d extension="$(pwd)/php/src/modules/opdump.so" -f /tmp/demo/app/index.php

Notice: Unknown: opdump: loaded /tmp/demo/out/index.php (6 opcodes, 4 literals, 0 dynamic funcs, 1 functions, 0 classes) without parsing source in Unknown on line 0
Hello, Bytecode!
```

The `Notice: ... loaded ... without parsing source` line is expected,
informational output from the loader confirming it actually intercepted
that file — a good thing to grep for while testing. It only appears because
`-n` (no php.ini) leaves `display_errors` in its CLI default; a production
php.ini can silence E_NOTICE without affecting the loader's behavior.

**Prove it never touched the real source** the way this project's own tests
do — replace the source with garbage first:

```bash
$ printf '<?php this is not valid php syntax !!! %%%%%%\n' > /tmp/demo/app/index.php
$ BYTECODE_KEY="$KEY" OPDUMP_MODE=load-tree OPDUMP_MAP=/tmp/demo/out/bytecode.map \
    php8.4 -n -d extension="$(pwd)/php/src/modules/opdump.so" -f /tmp/demo/app/index.php
Hello, Bytecode!
```

Still works — the loader never read the (now broken) `.php` file at all.

For a single file without a map, `OPDUMP_MODE=load` + `OPDUMP_IN=<path>`
loads exactly one container as the entrypoint (used for debugging, not for
whole-tree deployments):

```bash
BYTECODE_KEY="$KEY" OPDUMP_MODE=load OPDUMP_IN=/tmp/demo/out/index.php \
  php8.4 -n -d extension="$(pwd)/php/src/modules/opdump.so" -f /tmp/demo/app/index.php
```

### 3.7 License mode (RSA-wrapped keys)

Use this when you (the vendor) encode a build for someone else (the
customer) to run, and you don't want to hand them a raw shared secret.

**Once, by whoever will *run* the app** — generate a keypair and send only
the public key back to whoever is encoding:

```bash
$ php8.4 php/bin/bytecode-license-keygen /tmp/demo/keys
wrote /tmp/demo/keys/license.key.pem (keep this with whoever runs the decoded app)
wrote /tmp/demo/keys/license.pub.pem (use with BYTECODE_LICENSE_PUBKEY at encode time)
```

`license.key.pem` (RSA-4096 private key, chmod 600) never leaves the
machine that will run the app. `license.pub.pem` is the only thing sent to
whoever encodes the build.

**Encode** — `BYTECODE_LICENSE_PUBKEY` replaces `BYTECODE_KEY` entirely; a
fresh random DEK is generated for this build and wrapped for that public
key:

```bash
$ BYTECODE_LICENSE_PUBKEY=/tmp/demo/keys/license.pub.pem PHP_BIN=php8.4 \
    php8.4 php/bin/bytecode-dump /tmp/demo/app /tmp/demo/out-license
/tmp/demo/app/index.php -> /tmp/demo/out-license/index.php
wrote /tmp/demo/out-license/bytecode.license.json (RSA-OAEP-SHA256-wrapped DEK)
```

Ship `/tmp/demo/out-license/` (containers + manifest + map + sig +
`bytecode.license.json`) to the customer. It's useless without their
private key.

**Run** — `OPDUMP_LICENSE_KEY_FILE` replaces `BYTECODE_KEY`:

```bash
$ OPDUMP_LICENSE_KEY_FILE=/tmp/demo/keys/license.key.pem \
    OPDUMP_MODE=load-tree OPDUMP_MAP=/tmp/demo/out-license/bytecode.map \
    php8.4 -n -d extension="$(pwd)/php/src/modules/opdump.so" -f /tmp/demo/app/index.php
Hello, Bytecode!
```

If the private key file doesn't match the public key the build was wrapped
for, this fails closed with a clear error — it does **not** silently fall
back to `BYTECODE_KEY` even if one happens to be set:

```
Fatal error: Unknown: opdump: no key material available for BYTC2 load (checked license and BYTECODE_KEY/OPDUMP_KEY) in Unknown on line 0
```

If your private key is passphrase-protected, set
`OPDUMP_LICENSE_KEY_PASSPHRASE` alongside `OPDUMP_LICENSE_KEY_FILE`.

By default the loader looks for `bytecode.license.json` next to
`OPDUMP_MAP`; override with `OPDUMP_LICENSE_FILE` if you keep it elsewhere
(needed for single-file `OPDUMP_MODE=load`, which has no map to infer a
directory from).

**Making the license rules tamper-proof — the vendor seal**

License constraints (`--expires-at`, `--domains`, `--ips`, `--machine-id`,
`--fingerprints`, …) live as plaintext in `bytecode.license.json`. On their
own they are only as trustworthy as that file: in license mode the customer
holds the RSA private key that unwraps the DEK, and `bytecode.manifest.sig`
is an HMAC keyed by that same material — so a determined customer could edit
`expires_at`, delete a `domains` rule, and re-sign the manifest. The **vendor
seal** closes that gap with an Ed25519 signature only *you* can produce.

**Once, by the vendor** — generate a signing keypair. Unlike the license
keypair, the *private* half stays with you and the *public* half is the
loader's trust anchor:

```bash
$ php8.4 php/bin/bytecode-vendor-keygen /tmp/demo/vendor
wrote /tmp/demo/vendor/vendor.sign.key.pem (KEEP SECRET -- sign builds via BYTECODE_VENDOR_SIGN_KEY)
wrote /tmp/demo/vendor/vendor.sign.pub.pem (loader trust anchor -- OPDUMP_VENDOR_PUBKEY_FILE)

Compile this raw public key into your loader for the strongest anchor:
  ./configure --with-opdump-vendor-pubkey=7fec2b2e...41ebd054 ...
```

**Encode with a seal** — add `BYTECODE_VENDOR_SIGN_KEY` (or `--vendor-sign-key`)
to any encode. It writes `bytecode.seal.json` covering the manifest, map, and
license together:

```bash
$ BYTECODE_LICENSE_PUBKEY=/tmp/demo/keys/license.pub.pem \
    BYTECODE_VENDOR_SIGN_KEY=/tmp/demo/vendor/vendor.sign.key.pem PHP_BIN=php8.4 \
    php8.4 php/bin/bytecode-dump --expires-at 2030-01-01 --domains app.client.com \
      /tmp/demo/app /tmp/demo/out-license
wrote /tmp/demo/out-license/bytecode.license.json (RSA-OAEP-SHA256-wrapped DEK)
wrote /tmp/demo/out-license/bytecode.seal.json (Ed25519 vendor seal, key_id cafd3996...)
```

**Authorize specific machines without RSA license mode** — signed seal policy
can bind ordinary vendor-secret/shared-key containers to runtime machine
identity. The policy fields live in `bytecode.seal.json`, are hashed into
`policy_sha256`, and that hash is included in the Ed25519 signature. Editing
`machine_id`, hostnames, fingerprints, expiry, or activation settings after
encoding breaks the seal and the loader refuses the package.

```bash
BYTECODE_VENDOR_SIGN_KEY=/tmp/demo/vendor/vendor.sign.key.pem \
PHP_BIN=php8.4 \
php8.4 php/bin/bytecode-dump \
  --machine-id customer-prod-01 \
  --expires-at 2030-01-01 \
  /tmp/demo/app /tmp/demo/out-bound
```

Run it only on the authorized machine:

```bash
BYTECODE_VENDOR_KEY_FILE=/var/www/bytecode/build/vendor-secret.key \
OPDUMP_VENDOR_PUBKEY_FILE=/tmp/demo/vendor/vendor.sign.pub.pem \
OPDUMP_MACHINE_ID=customer-prod-01 \
OPDUMP_MODE=load-tree \
OPDUMP_MAP=/tmp/demo/out-bound/bytecode.map \
php8.4 -d zend_extension=/var/www/bytecode/php/src/modules/opdump.so /tmp/demo/app/index.php
```

With a different or missing `OPDUMP_MACHINE_ID`, the loader fails closed before
executing protected code.

**Anchor the loader.** Give the loader your public key, two ways (compiled-in
wins when both are present):

- **Compiled in** (strongest — the anchor lives inside your custom `.so` and
  can't be swapped on the server): build with
  `./configure --with-opdump-vendor-pubkey=<64hex>` using the hex printed by
  `bytecode-vendor-keygen`.
- **Server file**: point `OPDUMP_VENDOR_PUBKEY_FILE` at `vendor.sign.pub.pem`.

Once a loader has an anchor it **fails closed**: any package whose manifest,
map, or license was altered after encoding — or that carries no seal at all —
refuses to load.

```bash
# Customer edits expires_at 2030 -> 2099 and redeploys:
$ sed -i 's/2030-01-01/2099-01-01/' /tmp/demo/out-license/bytecode.license.json
$ OPDUMP_LICENSE_KEY_FILE=/tmp/demo/keys/license.key.pem \
    OPDUMP_VENDOR_PUBKEY_FILE=/tmp/demo/vendor/vendor.sign.pub.pem \
    OPDUMP_MODE=load-tree OPDUMP_MAP=/tmp/demo/out-license/bytecode.map \
    php8.4 -n -d extension="$(pwd)/php/src/modules/opdump.so" -f /tmp/demo/app/index.php
Warning: opdump: bytecode.license.json digest does not match seal ... -- refusing to load
```

Loaders **without** an anchor ignore the seal entirely, so existing builds and
the shared-key/vendor-key workflows keep working unchanged. Verify a seal
before shipping with `BYTECODE_VENDOR_PUBKEY=<pub.pem> bytecode-verify <dir>`.

### 3.8 Variable-name obfuscation

`--obfuscate` renames non-parameter local variables to `_v0`, `_v1`, ... in
the dumped bytecode. It's safe because compiled opcodes reference locals by
integer slot index, not name — the rename is pure metadata with zero effect
on control flow. It's automatically skipped, per function, whenever
`compact()`/`extract()`/`get_defined_vars()`/`$$name`/dynamic call dispatch
is detected in that function, so it never risks changing behavior (running
`bytecode-scan` first tells you *why* a given function got skipped).

```bash
BYTECODE_KEY="$KEY" php8.4 php/bin/bytecode-dump --obfuscate /tmp/demo/app /tmp/demo/out
```

Off by default — it's a real, if small, observable change (e.g. what
`debug_backtrace()` reports for a local's name), so it shouldn't silently
change output for anyone not opting in.

### 3.9 The `bytecode.json` config file

Instead of repeating `--exclude`/`--scan`/`--fail-on-scan-warning` flags
every time, put them in a config file. `bytecode-dump` and `bytecode-scan`
both read it — pass `--config <path>` explicitly, or drop a
`bytecode.json` in your current directory and it's picked up automatically.

```json
{
  "exclude": ["vendor/*", "storage/*", ".git/*", "node_modules/*"],
  "assets": {
    "include": false
  },
  "scanner": {
    "enabled": true,
    "fail_on_warning": false,
    "ignore_codes": ["dynamic-dispatch"]
  }
}
```

| Field | Type | Effect |
|---|---|---|
| `exclude` | `string[]` | Glob patterns, merged with any `--exclude` flags. |
| `assets.include` | `bool` | Same as passing `--include-assets` to `bytecode-dump`. |
| `scanner.enabled` | `bool` | Same as passing `--scan` to `bytecode-dump`. |
| `scanner.fail_on_warning` | `bool` | Same as `--fail-on-scan-warning` / `bytecode-scan --fail-on-warning`. |
| `scanner.ignore_codes` | `string[]` | Warning codes (see [§3.2](#32-scan-source-for-risky-dynamic-constructs)) to silently drop from the report. |

```bash
php8.4 php/bin/bytecode-dump --config bytecode.json app /tmp/demo/out
# or, from a directory containing bytecode.json:
cd app && php8.4 /path/to/php/bin/bytecode-dump . /tmp/demo/out
```

### 3.10 Installing the loader system-wide

For a long-running install (not just one-off CLI testing), build and
register the extension against a real PHP install with
`bytecode-install-loader`:

```bash
$ php8.4 php/bin/bytecode-install-loader --php-version 8.4 --build-only
== Build Bytecode PHP Loader for PHP 8.4 ==
...
Built loader:
/var/www/bytecode/php/src/modules/opdump.so

Build-only mode complete.
Test with:
php8.4 -n -d zend_extension=/var/www/bytecode/php/src/modules/opdump.so -v
```

`--build-only` only builds — nothing on the system changes. Confirm it
registers correctly:

```bash
$ php8.4 -n -d zend_extension=/var/www/bytecode/php/src/modules/opdump.so -v
PHP 8.4.23 (cli) ...
Zend Engine v4.4.23, Copyright (c) Zend Technologies
    with Bytecode PHP Loader v0.0.1phase2, Copyright (c) 2026 ..., by ...
```

That "with Bytecode PHP Loader ..." line is the confirmation — the same
`.so` also has a plain PHP-module entry point (used everywhere else in this
guide via `-d extension=...`), but loading it as a **Zend extension**
(`zend_extension=...`) is what makes it list itself like OPcache/ionCube do,
which is the shape you want for a permanent install.

Drop `--build-only` to actually install it (writes a `mods-available` ini on
Debian-family systems and runs `phpenmod`; asks for `sudo` unless run as
root or with `--no-sudo`):

```bash
sudo php8.4 php/bin/bytecode-install-loader --php-version 8.4 --sapi cli,fpm
```

`--dry-run` prints every command it would run without executing anything —
use it first to see exactly what it's going to touch.

The installer replaces the `.so` **atomically** (stages `<name>.new`, then
renames it over the target) and then **restarts** the services for the `fpm`
and `apache2` SAPIs you passed to `--sapi` (e.g. `php8.4-fpm`, `apache2`) so
they load the new loader. This matters: overwriting the `.so` in place while a
running php-fpm/apache still has it memory-mapped corrupts that process and
segfaults its workers (a site-wide `503` until restart). Pass `--no-restart`
to manage the restart yourself; `cli` never needs one.

**Two gotchas specific to a system install, both confirmed while writing
this guide:**

1. **Always use an absolute path for `zend_extension=`/`extension=`.** A
   relative path resolves against PHP's compiled-in `extension_dir`, not
   your current directory — if it can't find the file there, PHP prints a
   `Warning: Failed loading Zend extension` and then **keeps running,
   silently falling back to compiling the real source**. If your app's
   source is still on disk (normal for a first test), the output looks
   *identical* whether the loader engaged or not — you will not notice
   anything is wrong from the output alone. `bytecode-install-loader`
   already writes the correct absolute path into the ini file it
   generates; only be careful if you're hand-writing `-d zend_extension=...`
   yourself.
2. **`OPDUMP_MODE`/`OPDUMP_MAP`/`BYTECODE_KEY`/`OPDUMP_LICENSE_KEY_FILE` are
   real process environment variables**, read via `getenv()` — not `php.ini`
   directives. Setting them with `php_admin_value` in php.ini does nothing.
   For PHP-FPM, set them in the pool's `env[...]` directives (and enable
   `clear_env = no` if your distro's FPM defaults to a scrubbed
   environment); for Apache/mod_php, use `SetEnv` (and confirm your
   distro's Apache doesn't already strip custom env vars); for a systemd
   service, use `Environment=`/`EnvironmentFile=` in the unit.

## 4. GUI walkthrough

The desktop app (`ui/`, Flutter) is a front end over the exact same CLI
tools documented above — every button just shells out to one of them and
streams its output into the log pane. Nothing it does is unavailable from
the CLI; it exists to make the common workflow (encode → verify → inspect)
faster to iterate on.

### Running it

From source, for development:

```bash
cd ui
flutter pub get
flutter run -d linux    # or -d macos / -d windows
```

Or use a built package — see [`README.md`](README.md#desktop-packages) for
building an AppImage/.deb/.app/.zip/.msi. Packaged builds bundle their own
PHP CLI and the `opdump` extension, so they work without a separate PHP
install; when run from source, the app falls back to your system `php`
binary (override with the `BYTECODE_PHP` environment variable) and needs
`php/src/modules/opdump.so` already built (§2).

### Layout

Two panels: **Build Setup** on the left (every input the CLI tools take),
**Artifacts** on the right (the manifest table + a live command-output log,
filled in after you run something).

### Build Setup, field by field

| Control | CLI equivalent |
|---|---|
| **Bytecode root** | The repo root containing `php/bin/*` — auto-detected (bundled root in a packaged build, else the app's working directory). Change it if you're pointing the GUI at a different checkout. |
| **Output folder** | The `<output-dir>` argument to `bytecode-dump`/where the manifest is read from for Verify. |
| **PHP version** field + wrench icon (**Build loader**) + download icon (**Install Zend loader**) | `bytecode-install-loader --php-version <ver> --build-only` / (without `--build-only`) — §3.10. |
| **Raw / Assets / Obfuscate / Scan / Fail warnings** chips | `--raw` / `--include-assets` / `--obfuscate` / `--scan` (Scan is **on by default** in the GUI) / `--fail-on-scan-warning`. **Assets** is off by default and disabled while **Raw** is selected. |
| **Add Files** / **Add Folder** | Builds up the `<source-file-or-dir>...` argument list — shown in the source list below with a remove (×) per entry and a **Clear** to empty it. |
| **bytecode.json** field | `--config <path>` — §3.9. |
| **Vendor secret key file** field + sparkle icon (**Regenerate vendor secret**) | `build/vendor-secret.key`, passed as `BYTECODE_VENDOR_KEY_FILE` for Dump. The file is auto-created by the CLI if missing; the sparkle icon runs `bytecode-keygen --vendor-secret --force`. Disabled when **Raw** is selected, since raw mode needs no key. |
| **License public key** field | `BYTECODE_LICENSE_PUBKEY` — set this (instead of a key) to dump in license mode; disabled under **Raw**. |
| **License key folder** field + key icon (**Generate license keys**) | `bytecode-license-keygen <dir>` — writes `license.key.pem`/`license.pub.pem` into that folder and auto-fills **License public key** with the resulting `license.pub.pem`. |
| **Vendor sign key** field | `BYTECODE_VENDOR_SIGN_KEY` — set this to write a `bytecode.seal.json` (Ed25519 vendor seal) alongside the build; disabled under **Raw**. When set, **Verify** also passes the sibling `vendor.sign.pub.pem` as `BYTECODE_VENDOR_PUBKEY` to check the seal signature. §3.7. |
| **Authorized machine ID** field | Adds `--machine-id <id>` to Dump/Deploy. With a vendor sign key and a runtime vendor public-key anchor, the loader requires matching `OPDUMP_MACHINE_ID`. |
| **Vendor key folder** field + shield icon (**Generate vendor signing keys**) | `bytecode-vendor-keygen <dir>` — writes `vendor.sign.key.pem`/`vendor.sign.pub.pem`, auto-fills **Vendor sign key**, and streams the `--with-opdump-vendor-pubkey` compile-in hex into the log pane. |
| **Exclude globs** field | Comma-separated list, split into repeated `--exclude` flags. Pre-filled with `.history/*, vendor/*, storage/*, node_modules/*`. |
| **Scan** button | Runs `bytecode-scan` standalone (no dump) over the current source list. |
| **Dump** button | Runs `bytecode-dump` with every option above, then loads the resulting manifest into the Artifacts table. |
| **Verify** button | Runs `bytecode-verify <output>/bytecode.manifest.json`. |
| **Inspect container** field + info icon | Runs `bytecode-info <path>` — auto-filled with the first manifest entry after a successful Dump. |
| **Package version** field + AppImage/deb/macOS/Win ZIP/MSI buttons | The desktop packaging scripts (`scripts/build-*`) — see [`README.md`](README.md#desktop-packages). |
| **Status banner** (bottom) | Last action's result or error, in plain text. |

### Artifacts panel

- **Manifest table**: one row per encoded file (source, output, size,
  SHA-256) — populated after **Dump** finishes successfully, read straight
  from `bytecode.manifest.json`.
- **Log pane**: raw stdout+stderr of whatever subprocess just ran, streamed
  live. This is where scan warnings, dump errors, or verify failures
  actually show up — check here first when the status banner just says
  something failed.

### A full example run through the GUI

1. Set **Bytecode root** to this repo's path (should auto-fill correctly if
   you launched the GUI from inside it).
2. **Add Folder** → pick your project's source directory.
3. Leave **Scan** checked, click **Scan** once by itself first if you want
   to review warnings before committing to a build.
4. Use the default **Vendor secret key file** (`build/vendor-secret.key`), or
   click the sparkle icon to regenerate it before encoding. Fill in **License
   public key** instead for license mode.
5. Set **Output folder** to wherever you want the build written.
6. Click **Dump**. Watch the log pane; when it finishes, the manifest table
   populates and **Inspect container** auto-fills with the first file.
7. Click **Verify** to confirm the build is internally consistent.
8. Click the info icon next to **Inspect container** to see that container's
   header fields.

Running the resulting build is not yet wired into the GUI — use the CLI
(§3.6/§3.7) to actually execute an encoded tree.

## 5. Troubleshooting

| Symptom | Likely cause |
|---|---|
| `opdump extension not found at ...` | `php/src/modules/opdump.so` isn't built — see §2, or set `OPDUMP_SO` to its actual path. |
| `BYTECODE_KEY/OPDUMP_KEY must be 64 hex chars` | Runtime key material is missing or malformed. For default vendor-secret builds, load with a loader compiled with the same vendor secret, or provide the key through `BYTECODE_VENDOR_KEY`. |
| `opdump: BYTC authentication/decryption failed` | Wrong key, or the container was tampered with — GCM authentication fails closed by design, before any payload is parsed. |
| `opdump: no key material available for BYTC2 load (checked license and BYTECODE_KEY/OPDUMP_KEY)` | In license mode: wrong `OPDUMP_LICENSE_KEY_FILE`, wrong passphrase, or `bytecode.license.json` not found at the expected path (see `OPDUMP_LICENSE_FILE` in §3.7). In shared-secret mode: `BYTECODE_KEY`/`OPDUMP_KEY` isn't set in the *loading* process's environment. |
| `opdump: bytecode.manifest.sig mismatch ... refusing to trust bytecode.map` | Either `bytecode.manifest.json`/`bytecode.map` was edited after the build, or you're pointing at a manifest/map/sig set that doesn't all belong together. |
| `opdump: ... digest does not match seal` / `no bytecode.seal.json ... refusing to load` | The loader has a vendor public-key anchor (`--with-opdump-vendor-pubkey` or `OPDUMP_VENDOR_PUBKEY_FILE`) so it requires a valid `bytecode.seal.json`. Re-encode with `--vendor-sign-key`, or a file was altered after sealing. Loaders with no anchor don't require a seal — see §3.7. |
| `opdump: BYTC PHP_VERSION_ID ... does not match running ...` | The container was built for a different PHP minor than the one loading it. Only PHP 8.4 is fully proven right now (see `docs/PLAN.md`) — build and run with the same `php8.4`. |
| Output looks correct even though you expected encoding to matter | If you loaded via `zend_extension=`/`extension=` with a **relative path** and it silently failed to load, PHP falls back to compiling the real source — see the gotcha in §3.10. Always use an absolute path, and confirm with `php -v` (for `zend_extension=`) that "Bytecode PHP Loader" is listed, or destroy the source file first the way §3.6 does, to get an unambiguous test. |
| Env vars have no effect under PHP-FPM/Apache | `OPDUMP_MODE` etc. are real process environment variables read via `getenv()`, not `php.ini` settings — see the second gotcha in §3.10. |
| `run-version-matrix.sh` / CI fails for PHP 8.1–8.3 | Known, documented gap — only PHP 8.4 is implemented so far (Phase 2 in `docs/PLAN.md` covers the rest). Not something you can currently fix by reconfiguring. |

## 6. Reference

### Environment variables

| Variable | Used by | Meaning |
|---|---|---|
| `PHP_BIN` | `bytecode-dump` | PHP binary to compile source with (default `php8.4`). |
| `OPDUMP_SO` | `bytecode-dump` | Path to `opdump.so`/`opdump.dll` (default `../src/modules/<name>` relative to `php/bin/`). |
| `BYTECODE_KEY`, `OPDUMP_KEY` | `bytecode-pack`, `bytecode-dump`, `bytecode-verify`, `opdump.so` | Optional shared-secret IKM override (64 hex chars), unless license mode is used. |
| `BYTECODE_VENDOR_KEY_FILE` | `bytecode-dump`, `bytecode-pack`, `bytecode-pack-asset`, `bytecode-verify`, asset runtime | Path to a 64-hex vendor-secret key file. Defaults to `build/vendor-secret.key`; encoding creates it automatically when missing. |
| `BYTECODE_DEK` | `bytecode-pack` | Internal — set automatically by `bytecode-dump` in license mode, takes priority over `BYTECODE_KEY` and the vendor-secret key file. Don't set this by hand. |
| `BYTECODE_LICENSE_PUBKEY` | `bytecode-dump` | Path to `license.pub.pem`; switches to license mode. |
| `BYTECODE_VENDOR_SIGN_KEY` | `bytecode-dump` | Path to the vendor Ed25519 private key (`bytecode-vendor-keygen`); writes `bytecode.seal.json`. Same as `--vendor-sign-key`. |
| `OPDUMP_VENDOR_PUBKEY_FILE` | `opdump.so`, `bytecode-verify` | Path to `vendor.sign.pub.pem`; the seal trust anchor when no key is compiled in (`--with-opdump-vendor-pubkey` wins over it). Present ⇒ loader fails closed on a missing/invalid seal. |
| `BYTECODE_VENDOR_PUBKEY` | `bytecode-verify` | Path to `vendor.sign.pub.pem` for full Ed25519 seal verification (falls back to `OPDUMP_VENDOR_PUBKEY_FILE`). |
| `OPDUMP_MACHINE_ID` | `opdump.so` | Runtime machine id checked against signed seal policy and/or `bytecode.license.json`. |
| `OPDUMP_LICENSE_KEY_FILE` | `opdump.so` | Path to `license.key.pem`; switches the loader to license mode. |
| `OPDUMP_LICENSE_KEY_PASSPHRASE` | `opdump.so` | Passphrase for an encrypted `license.key.pem`, if any. |
| `OPDUMP_LICENSE_FILE` | `opdump.so` | Path to `bytecode.license.json`; default is alongside `OPDUMP_MAP`. |
| `OPDUMP_MODE` | `opdump.so` | `dump`, `load`, or `load-tree`. |
| `OPDUMP_OUT` | `opdump.so` (dump mode) | Where to write the raw `.opd2` blob. |
| `OPDUMP_OBFUSCATE` | `opdump.so` (dump mode) | Set (to any value) to enable variable-name obfuscation — set by `bytecode-dump --obfuscate`, not usually by hand. |
| `OPDUMP_IN` | `opdump.so` (load mode) | Path to one encoded container (or `.opd2` in `--raw` mode) to load as the entrypoint. |
| `OPDUMP_MAP`, `BYTECODE_MAP` | `opdump.so` (load-tree mode) | Path to `bytecode.map`. |
| `OPDUMP_DEBUG` | `opdump.so` | Set to dump verbose opcode/literal debug output to stderr. |
| `BYTECODE_PHP` | GUI | PHP binary the GUI shells out to, when running from source. |
| `BYTECODE_ROOT` | GUI | Overrides the auto-detected repo root. |

### Files written by `bytecode-dump`

| File | Contents |
|---|---|
| `<name>.<ext>` | One encrypted container per included source file, at the same relative path and filename the source had. PHP files contain Zend bytecode; HTML/CSS/JS/Twig/Blade files contain encrypted raw asset bytes only when `--include-assets` is enabled. (`--raw` writes PHP debug blobs as `<name>.php.opd2` instead.) |
| `bytecode.manifest.json` | Per-file source/output/hash/size list, PHP version, container format, and (if `--scan`) embedded scan results. |
| `bytecode.map` | `absolute-source-path<TAB>relative-container-path`, one per line — the loader's runtime lookup table. |
| `bytecode.manifest.sig` | Hex HMAC-SHA256 over manifest + map, keyed by an HKDF derivation of your IKM/DEK. |
| `bytecode.license.json` | Only in license mode: the RSA-OAEP-SHA256-wrapped DEK for this build, plus any license constraints. |
| `bytecode.seal.json` | Only with `--vendor-sign-key`/`BYTECODE_VENDOR_SIGN_KEY`: an Ed25519 vendor signature over the SHA-256 digests of manifest + map + license, verified by a loader that has your public key. |

### Exit codes

All CLI tools: `0` success, `2` usage error (bad flags/args), `1` operational
failure (build/pack/verify/scan failed). `bytecode-scan`/`bytecode-dump
--fail-on-scan-warning` exit `1` specifically when warnings were found, even
though scanning itself succeeded.
