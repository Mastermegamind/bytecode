# Phase 4 — OPcache, Packaging, Keys, Anti-Tamper

Phase 4 is the productization phase. It is blocked until Phase 0-3 prove the
loader is semantically trustworthy.

## Tracks

- OPcache behavior: decide whether loaded op_arrays participate in shared
  memory caching or bypass it.
- Packaging: produce one loader build per PHP minor version and platform.
- Key model: replace `BYTECODE_KEY` environment loading with a real key source.
- Licensing hooks: design separately from bytecode correctness.
- Anti-tamper: authenticate containers before touching Zend structures.

## Current State

Phase 1 already authenticates/decrypts `BYTC1` containers with AES-256-GCM
using a 32-byte key from `BYTECODE_KEY` or `OPDUMP_KEY`. That is a development
key path only, not the final Phase 4 model.

See [`PHASE4_KEY_MODEL.md`](PHASE4_KEY_MODEL.md) for the early key/provider
and container-integrity constraints that should shape the Phase 1/2 format
before product licensing is implemented.

## Done Criteria

1. Tampered containers fail before payload parsing.
2. Wrong PHP versions fail before execution.
3. Packages install the correct loader for the target PHP ABI.
4. OPcache behavior is explicitly tested and documented.
5. Key handling no longer depends on plain environment variables for production.
