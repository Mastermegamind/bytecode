# Contributing

## Building

```bash
cd php/src
phpize8.4
./configure --enable-opdump --with-php-config=/usr/bin/php-config8.4
make
cd ../..
```

macOS and other PHP minors: see the build shapes in [`README.md`](README.md).

## Running the tests

```bash
php/tests/run-rung1.sh
php/tests/run-rung2.sh
php/tests/run-rung3.sh
php/tests/run-rung4.sh
php/tests/run-rung5-reflection.sh
php/tests/run-load-tree.sh
php/tests/run-phase1-container.sh
php/tests/run-corrupt-container.sh
php/tests/run-rung6-hkdf.sh
php/tests/run-rung7-license.sh
php/tests/run-rung8-manifest-auth.sh
php/tests/run-rung9-obfuscate.sh
```

Each is a self-contained bash script: it builds fixtures in a temp dir,
exercises one specific behavior, and exits non-zero on failure. Run the
whole set before opening a PR.

## Where to start

- [`docs/PLAN.md`](docs/PLAN.md) — the phased roadmap and current status.
- [`docs/PHASE0_STATUS.md`](docs/PHASE0_STATUS.md) — the Zend-internals
  gotchas already found the hard way (constant addressing, opcode handler
  ordering, class linking, etc.) — read this before touching
  `php/src/opdump.c`'s dump/load paths, since it documents *why* the code
  looks the way it does in several non-obvious places.
- [`docs/PHASE4_KEY_MODEL.md`](docs/PHASE4_KEY_MODEL.md) — the container
  format and key-handling model, if you're touching crypto/container code.

## Ground rules

- New Zend-internals behavior (a new opcode kind, a new literal type, a new
  class/property feature) needs a test that proves dump→reload→execute
  parity, following the existing rung pattern
  (`php/tests/run-rungN-*.sh` + a small subject `.php` file).
- Anything touching key handling, container format, or the crypto path
  needs an explicit fail-closed test (see `run-corrupt-container.sh` and
  `run-rung8-manifest-auth.sh` for the pattern: prove the loader refuses
  bad input *before* it does anything else, not just that good input works).
- Don't add opcode-level transforms (reordering, dummy-opcode injection) --
  see the Non-Goals in `docs/PHASE4_KEY_MODEL.md` for why that's
  deliberately out of scope.
