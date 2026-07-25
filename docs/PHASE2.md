# Phase 2 — PHP Version Matrix

Phase 2 ports the proven PHP 8.4 backend to PHP 8.1, 8.2, 8.3, and 8.5. Phase
0 rungs 1–5 now pass on PHP 8.4, so the active next step is making the same
build/reload semantics hold across older supported Zend layouts.

## Goal

For each PHP minor version:

- build `opdump.so` against that version's `phpize` and `php-config`
- run the Phase 0/1 round-trip tests
- document struct layout changes that require version-specific handling
- keep every version's blob guarded by `PHP_VERSION_ID`

## Runner

`php/tests/run-version-matrix.sh` builds the extension against each requested
PHP minor and runs the current rung/container suite:

```bash
php/tests/run-version-matrix.sh
```

Limit a local run to one PHP minor while debugging a port:

```bash
PHP_VERSIONS=8.1 php/tests/run-version-matrix.sh
```

GitHub Actions runs the same script for PHP 8.1, 8.2, 8.3, and 8.4 in
`.github/workflows/php-version-matrix.yml`.

PHP 8.5 remains in the roadmap, but it is intentionally not wired into CI
until the version is available in the hosted runner/tooling channel.

## Current Test Coverage

The matrix currently runs:

- `run-rung1.sh`
- `run-rung2.sh`
- `run-rung3.sh`
- `run-rung4.sh`
- `run-rung5-reflection.sh`
- `run-load-tree.sh`
- `run-phase1-container.sh`

The first expected Phase 2 failures should be treated as porting data, not as
test noise. PHP 8.1 is the highest-risk spike because enums, readonly
properties, fibers, `never`, and intersection types landed there.

## Done Criteria

1. PHP 8.1, 8.2, 8.3, 8.4, and 8.5 all build the extension.
2. Each version passes the full Phase 0 rung ladder.
3. Each version can load its own Phase 1 `BYTC1` containers.
4. Cross-version loads fail cleanly before execution.
5. Version-specific notes are recorded in this file or per-version references.
