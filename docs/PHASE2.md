# Phase 2 — PHP Version Matrix

Phase 2 ports the proven PHP 8.4 backend to PHP 8.1, 8.2, 8.3, and 8.5. It is
blocked until Phase 0 rungs 3–5 pass on PHP 8.4.

## Goal

For each PHP minor version:

- build `opdump.so` against that version's `phpize` and `php-config`
- run the Phase 0/1 round-trip tests
- document struct layout changes that require version-specific handling
- keep every version's blob guarded by `PHP_VERSION_ID`

## Runner

`php/tests/run-version-matrix.sh` is the starting point. By default it refuses
to run because Phase 0 is not complete:

```bash
php/tests/run-version-matrix.sh
```

When the Phase 0 go/no-go has passed:

```bash
PHASE0_GO=1 php/tests/run-version-matrix.sh
```

## Done Criteria

1. PHP 8.1, 8.2, 8.3, 8.4, and 8.5 all build the extension.
2. Each version passes the full Phase 0 rung ladder.
3. Each version can load its own Phase 1 `BYTC1` containers.
4. Cross-version loads fail cleanly before execution.
5. Version-specific notes are recorded in this file or per-version references.
