# Encoding & obfuscation — demo and commands

A worked example for encoding and obfuscating a PHP file through all three
front-ends (CLI, HTTP API, MCP), using the sample source at
[`examples/demo.php`](../examples/demo.php).

## The sample file

`examples/demo.php` is runnable on its own and mixes constructs so each feature
is visible:

| construct in the file | what it demonstrates |
|-----------------------|----------------------|
| local variables (`$normalized`, `$running`, `$value`) | renamed to random names by `--obfuscate` |
| a typed property with a `/** … */` docblock | property doc-comment stripped in the bytecode |
| `summarize()` using `compact()` | obfuscation **fails closed** and skips the function (renaming would break `compact()`) |
| `dynamicLookup()` using `$$field` | `scan` warns; the obfuscator skips the function |

Run it before and after encoding — the output must be identical:

```
$ php examples/demo.php
Acme Corp: 125.25 (due)
lookup: A-1
```

## Prerequisites

- A dump-capable **opdump** extension built for *your exact* PHP version must be
  available. Encoding (`dump`) needs it; `keygen`, `pack`, `verify`,
  `package-sign` do not. Two things must line up:
    1. **Dumping** — `bytecode-dump` auto-selects a dump-capable loader that
       matches the target `php` (it prefers a freshly built
       `php/src/modules/opdump.so`, then `build/loaders/opdump-php<MAJ.MIN>.so`,
       verifying each can actually emit `OPD2`). Override with `--opdump-so` /
       `OPDUMP_SO`.
    2. **Running** encoded output — the runtime loader is checked against the
       container's stamped `PHP_VERSION_ID`, which is **patch-sensitive**
       (e.g. `8.4.27` → `80427`). The loader must be built for the same patch
       release as the PHP that runs the encoded code, or you get
       `PHP_VERSION_ID NNNNN does not match running MMMMM`.
  Build a matching extension against your installed PHP:

  ```bash
  cd php/src
  phpize8.4 --clean && phpize8.4
  ./configure --with-php-config=/path/to/php-config8.4   # must report your exact PHP
  make -j
  # dumper: php/src/modules/opdump.so   (bytecode-dump picks it up automatically)
  # runtime loader: install that .so into the extension dir and enable it
  #   (bytecode install-loader), or run with -d zend_extension=<that opdump.so>
  ```

  `bytecode doctor` reports the pieces.
- A 32-byte key (64 hex). Generate one and export it:

```bash
export BYTECODE_KEY="$(rust/target/release/bytecode keygen)"
```

All paths below are relative to the repo root; run the commands from there (for
the API/MCP servers, start them from the repo root or use absolute paths).

## CLI

```bash
# 1. Plain encode -> encrypted BYTC containers + manifest/map/signature
bytecode dump --out out examples/demo.php

# 2. Safe bytecode-level obfuscation + a hardening preset + a preflight scan
bytecode dump --obfuscate --obfuscate-profile balanced --scan --out out examples/demo.php

# 3. Source-level obfuscation first, then encode
bytecode dump --workflow obfuscate-then-encode --obfuscate --out out examples/demo.php

# 4. One-shot pipeline: scan -> dump -> verify (args after `--` go to dump)
bytecode encode --scan examples/demo.php --verify-dir out --decrypt-test \
  -- --obfuscate --out out examples/demo.php

# 5. Verify the output (structure + decrypt every container)
bytecode verify --decrypt-test out
```

`bytecode` is `rust/target/release/bytecode`. Obfuscation knobs:
`--obfuscate-mode identifier|hexa|numeric`, `--obfuscate-length <n>`,
`--obfuscate-seed <s>` (reproducible), `--obfuscate-profile light|balanced|paranoid`.

## HTTP API

```bash
export BYTECODE_API_TOKEN="$(openssl rand -hex 16)"
bytecode api 127.0.0.1:8787 &            # or: php/bin/bytecode-api
H="Authorization: Bearer $BYTECODE_API_TOKEN"

# encode + obfuscate (one tool call)
curl -H "$H" -X POST localhost:8787/tools/dump -d '{
  "args": ["--obfuscate","--obfuscate-profile","balanced","--scan","--out","out","examples/demo.php"],
  "env": {"BYTECODE_KEY": "'"$BYTECODE_KEY"'"}
}'

# one-shot pipeline
curl -H "$H" -X POST localhost:8787/tools/encode -d '{
  "dump_args": ["--obfuscate","--out","out","examples/demo.php"],
  "scan_paths": ["examples/demo.php"],
  "verify_dir": "out",
  "decrypt_test": true,
  "env": {"BYTECODE_KEY": "'"$BYTECODE_KEY"'"}
}'

# verify
curl -H "$H" -X POST localhost:8787/tools/verify -d '{
  "args": ["--decrypt-test","out"],
  "env": {"BYTECODE_KEY": "'"$BYTECODE_KEY"'"}
}'
```

Only `BYTECODE_*` / `OPDUMP_*` / `PHP_BIN` env keys are accepted, and they are
visible via `/proc` to the same OS user — this is a single-operator local server.

## MCP

Point an MCP client at `php/bin/bytecode-mcp` (or `bytecode mcp`) and call the
tools. The raw JSON-RPC for a quick stdio test:

```bash
printf '%s\n' \
 '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
 '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"dump","arguments":{"args":["--obfuscate","--obfuscate-profile","balanced","--out","out","examples/demo.php"],"env":{"BYTECODE_KEY":"'"$BYTECODE_KEY"'"}}}}' \
 '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"encode","arguments":{"dump_args":["--obfuscate","--out","out","examples/demo.php"],"scan_paths":["examples/demo.php"],"verify_dir":"out","decrypt_test":true,"env":{"BYTECODE_KEY":"'"$BYTECODE_KEY"'"}}}}' \
 | php/bin/bytecode-mcp
```

Every tool accepts `{ "args":[…], "stdin"?:"…", "env"?:{…} }`; the `encode`
tool accepts `{ "dump_args":[…], "scan_paths"?:[…], "verify_dir"?:"…",
"decrypt_test"?:bool, "env"?:{…} }`. Each tool's `--help` is included in its
`tools/list` description.

## GPU or CPU for encoding/obfuscation?

**Use the CPU. A GPU would not help here** — it would add complexity for no gain,
and usually a net loss. Why:

- **Encoding** is Zend **compilation** (parse → AST → opcodes). It is inherently
  serial, branch-heavy and pointer-chasing per file — the opposite of the wide,
  uniform, data-parallel work GPUs are good at. It cannot be offloaded.
- **Obfuscation** is AST/bytecode **symbol rewriting** on small, irregular
  structures — also control-flow-bound, not GPU work.
- **Encryption** (AES-256-GCM) and key derivation (HKDF/HMAC-SHA256) run on
  per-file payloads that are kilobytes. Modern CPUs have **AES-NI** and SHA
  hardware instructions that encrypt these far faster than it would take to ship
  the data to a GPU and back. GPUs only win for *huge* contiguous bulk with
  batching that amortizes transfer latency — which this workload is not.
- The real cost on a large codebase is **per-file PHP process spawn + compile +
  I/O**, which is latency- and CPU-bound.

**What actually speeds it up (all CPU):**

1. **Parallelize across files** — run N encode workers for N cores. Encoding one
   file is serial, but a 5,000-file project is embarrassingly parallel across
   files. (Agents can fan this out over the CLI/API/MCP.)
2. **Rely on AES-NI** — OpenSSL and the Rust `aes-gcm` crate already use it; no
   action needed beyond a modern CPU.
3. **Keep I/O fast** — encode to a local SSD / tmpfs, not a network mount.

Bottom line: pick a machine with **more/faster CPU cores**, not a GPU.
