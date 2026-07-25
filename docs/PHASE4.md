# Phase 4 — OPcache, Packaging, Keys, Anti-Tamper

Phase 4 is the productization phase, meant to be blocked until Phase 0-3
prove the loader is semantically trustworthy across PHP versions. The key
model and anti-tamper track (this doc + `PHASE4_KEY_MODEL.md`) landed ahead
of that gate at the user's request, since it's a container-format change
best made before more Phase 2 per-version work builds on top of it -- it
does not depend on Phase 2's multi-version matrix, only on the Phase 1
container shape. OPcache behavior and per-version packaging are still
untouched and still blocked on Phase 2/3.

## Tracks

- OPcache behavior: decide whether loaded op_arrays participate in shared
  memory caching or bypass it. **Not started.**
- Packaging: produce one loader build per PHP minor version and platform.
  **Not started** -- still blocked on Phase 2 (see `docs/PLAN.md`;
  `php/tests/run-version-matrix.sh` fails today for PHP 8.1-8.3, which is
  the known, documented Phase 2 gap, not a Phase 4 regression).
- Key model: replace `BYTECODE_KEY` environment loading with a real key
  source. **Done** -- see `PHASE4_KEY_MODEL.md`.
- Licensing hooks: design separately from bytecode correctness. **Done** for
  key wrapping (RSA-OAEP-SHA256-wrapped DEK); license expiry/revocation is
  still open, see `PHASE4_KEY_MODEL.md`'s Open Questions.
- Anti-tamper: authenticate containers before touching Zend structures.
  **Done** for both container payloads (already true since Phase 1) and now
  `bytecode.manifest.json`/`bytecode.map` (new in `BYTC2`, see
  `PHASE4_KEY_MODEL.md`).

## Current State

`BYTC2` containers derive their AES key via HKDF-SHA256 from either a shared
`BYTECODE_KEY`/`OPDUMP_KEY` secret or an RSA-OAEP-SHA256-unwrapped
per-build DEK (license mode), authenticate `bytecode.manifest.json` +
`bytecode.map` together via `bytecode.manifest.sig`, and apply best-effort
in-memory hardening (cleanse, mlock, `RLIMIT_CORE=0`) to key material in the
loader. `BYTC1` (the original Phase 1 shared-secret format, no HKDF, no
manifest authentication) remains a supported read path.

See [`PHASE4_KEY_MODEL.md`](PHASE4_KEY_MODEL.md) for the full detail on what
shipped, what was deliberately left out (loader self-checksum, anti-debug,
opcode-level obfuscation), and what's still open.

## Done Criteria

1. Tampered containers fail before payload parsing. **Done** (Phase 1, GCM
   auth) -- and now `bytecode.manifest.json`/`bytecode.map` tampering is
   caught too, before any container is even opened.
2. Wrong PHP versions fail before execution. **Done** (`PHP_VERSION_ID`
   guard; a real ABI/build-id guard beyond that is still open, see
   `PHASE4_KEY_MODEL.md`).
3. Packages install the correct loader for the target PHP ABI. **Not
   started** -- blocked on Phase 2.
4. OPcache behavior is explicitly tested and documented. **Not started.**
5. Key handling no longer depends on plain environment variables for
   production. **Done** for the license-mode path (RSA-wrapped DEK); the
   env-secret path remains supported for local/self-hosted use, by design.
