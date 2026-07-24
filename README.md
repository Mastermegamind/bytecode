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

php/bin/bytecode-keygen
php/bin/bytecode-info /tmp/bytecode-out/rung1.php.bytc
php/bin/bytecode-verify /tmp/bytecode-out/bytecode.manifest.json

BYTECODE_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef \
  php/bin/bytecode-dump php/tests /tmp/bytecode-out

BYTECODE_KEY=... OPDUMP_MODE=load-tree OPDUMP_MAP=/tmp/bytecode-out/bytecode.map \
  php8.4 -n -d extension=php/src/modules/opdump.so -f path/to/entrypoint.php
```

## License

MIT — see [`LICENSE`](LICENSE).
