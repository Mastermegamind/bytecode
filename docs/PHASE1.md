# Phase 1 — Container Format + CLI Dumper

Phase 1 builds on the Phase 0 PHP 8.4 spike. Rungs 1–5 now pass on PHP 8.4;
cross-version support remains gated by Phase 2.

## Goal

Turn the Phase 0 raw opcode blob into a production-shaped artifact:

- a versioned container header
- a PHP-version/build-id guard
- an AES-256-GCM encrypted payload
- a CLI tool that walks a source tree and emits encoded files
- loader support for reading the encrypted container instead of only the raw
  Phase 0 blob
- recursive tree loading through a source-path-to-container map, so includes
  and Composer autoload requests can resolve encoded files one at a time

## Current Implementation

`php/bin/bytecode-dump` is a tree-walking dumper wrapper around the Phase 0
extension. It uses `php8.4 -l` so files are compiled but not executed, captures
the raw `OPD2` blob through `OPDUMP_MODE=dump`, and writes encrypted `BYTC1`
containers by default.

`php/bin/bytecode-pack` performs the AES-256-GCM container wrapping. The loader
accepts `BYTC1` containers in `OPDUMP_MODE=load`, decrypts them with
`BYTECODE_KEY` or `OPDUMP_KEY`, validates `PHP_VERSION_ID`, then dispatches the
decrypted `OPD2` payload through the existing backend reader.

`php/bin/bytecode-dump` also writes `bytecode.manifest.json` and
`bytecode.map` next to encoded outputs. The manifest records source root,
source path, output path, PHP version, backend, container type, SHA-256, and
file size. The map is the runtime hot path: absolute source filename to
relative encoded container path, separated by a tab.

Supporting tools:

```bash
php/bin/bytecode-keygen
php/bin/bytecode-info file.bytc
php/bin/bytecode-verify bytecode.manifest.json
BYTECODE_KEY=... OPDUMP_MODE=load-tree OPDUMP_MAP=/encoded/bytecode.map \
  php8.4 -n -d extension=php/src/modules/opdump.so -f /app/public/index.php
```

Raw `OPD2` output remains available for spike/debug work:

```bash
php/bin/bytecode-dump --raw php/tests /tmp/out
```

## Proposed Container

Binary layout:

```text
BYTC1
u32 container_version
u32 php_version_id
u32 backend_id_len
bytes backend_id          # e.g. php-zend-opdump
u32 backend_format_len
bytes backend_format      # e.g. OPD2
u32 nonce_len
bytes nonce
u32 tag_len
bytes tag
u64 ciphertext_len
bytes ciphertext
```

Payload:

```text
raw Phase 0 backend blob, encrypted with AES-256-GCM
```

Additional authenticated data:

```text
all header bytes before nonce/tag/ciphertext payload bytes
```

## Done Criteria

1. `bytecode-dump` walks a tree and emits encrypted container files. **Done.**
2. Loader accepts `BYTC1` containers and decrypts to the backend payload.
   **Done.**
3. Loader rejects wrong PHP versions/build IDs before execution. **Done for
   `PHP_VERSION_ID`; build-id guard remains future work.**
4. Existing Phase 0 rungs still pass through the container path. **Done for
   rungs 1–3 with `php/tests/run-phase1-container.sh`; recursive loading is
   covered by `php/tests/run-load-tree.sh`.**
5. Raw `OPD2` support remains available only as a debug/spike compatibility
   mode. **Done.**

## Verification

```bash
php/tests/run-rung1.sh
php/tests/run-rung2.sh
php/tests/run-rung3.sh
php/tests/run-rung4.sh
php/tests/run-rung5-reflection.sh
php/tests/run-load-tree.sh
php/tests/run-phase1-container.sh
```

Wrong-key loads fail during AES-GCM authentication before the decrypted payload
is parsed.
