# Phase 4 Key And Integrity Model

This note originally captured constraints meant to influence the Phase 1/2
container work before production key handling existed. The "Candidate Shape"
below is now implemented as `BYTC2`; `BYTC1` (raw shared-secret AES key, no
HKDF, no manifest authentication) remains a supported read path for anything
encoded before this landed.

## Development Model (`BYTC1`, legacy)

- `BYTC1` containers are encrypted and authenticated with AES-256-GCM.
- The key is 32 bytes supplied as 64 hex characters through `BYTECODE_KEY` or
  `OPDUMP_KEY`, used directly as the AES key.
- The loader rejects wrong keys or tampered ciphertext during GCM
  authentication before parsing the decrypted Zend payload.
- `bytecode.map`/`bytecode.manifest.json` are not authenticated in this
  format -- each container's own GCM tag only covers that container's own
  bytes, not the map wiring source paths to containers.

## Production Constraints

- Key lookup must be separable from Zend payload parsing. A failed key lookup,
  wrong key, expired license, or tampered container should all fail before any
  reconstructed op_array/class/function structures are allocated for execution.
- Container headers must keep enough unauthenticated metadata to select the
  correct loader/key path, but all execution-relevant metadata must be covered
  by authentication.
- The PHP minor/ABI guard must be checked before execution. Cross-version loads
  should fail with a clear diagnostic rather than attempting best-effort
  reconstruction.
- The same encoded artifact may need to run across several hosts. Avoid designs
  that require embedding a host-local secret directly into every container.
- Licensing checks must not be mixed into bytecode correctness tests. The Phase
  0–3 suites should continue to prove semantic parity independent of license
  policy.

## Implemented (`BYTC2`)

1. **Header.** `container_version = 2` adds `cipher_id` (currently always 1 =
   AES-256-GCM) and `key_id` (16 random bytes, not secret) to the existing
   plaintext-but-authenticated header, ahead of nonce/tag/ciphertext. All of
   it is AAD -- covered by the GCM tag, not just the ciphertext.
2. **Key derivation.** `BYTECODE_KEY`/`OPDUMP_KEY` (or a license-unwrapped
   DEK, see below) is treated as HKDF-SHA256 input keying material, not the
   AES key directly. The actual per-container key is
   `HKDF-SHA256(IKM, salt=key_id, info="bytecode-v2")`, so every container
   gets a distinct derived key even under one shared secret. See
   `opdump_hkdf_sha256()`/`opdump_resolve_ikm()` in `php/src/opdump.c` and
   the matching `hash_hkdf()` call in `php/bin/bytecode-pack`.
3. **Key-provider pluggability (the "selected by key id" idea from the
   original candidate shape) turned out simpler in practice: rather than a
   provider registry keyed by `key_id`, there are exactly two IKM sources,
   tried in a fixed order by `opdump_resolve_ikm()`:**
   - License mode (`OPDUMP_LICENSE_KEY_FILE` set): RSA-OAEP-SHA256-unwrap a
     DEK from `bytecode.license.json` using the given private key. Failure
     here does **not** fall back to the env path -- a misconfigured license
     fails loudly rather than silently downgrading to a dev key.
   - Env mode (`BYTECODE_KEY`/`OPDUMP_KEY`): used directly as IKM, as before.

   `bytecode-license-keygen` generates the RSA-4096 keypair;
   `BYTECODE_LICENSE_PUBKEY` on `bytecode-dump` generates one random DEK per
   build and wraps it with the public key into `bytecode.license.json`;
   `OPDUMP_LICENSE_KEY_FILE` on the loader unwraps it, caching the result for
   the process lifetime (RSA-OAEP is comparatively expensive, and load-tree
   mode may decrypt many containers per request).
4. **Manifest/map authentication** (closes the first Open Question below).
   `bytecode-dump` writes `bytecode.manifest.sig`: an HMAC-SHA256 over
   `bytecode.manifest.json || 0x00 || bytecode.map`, keyed by
   `HKDF-SHA256(IKM, salt="", info="bytecode-manifest-auth")`. `load-tree`
   mode verifies it via `opdump_verify_manifest_signature()` before trusting
   a single map entry, so repointing one source path at a different
   (validly-encrypted) container is now detected and refused, not silently
   loaded.
5. **In-memory hardening.** Key material buffers are `OPENSSL_cleanse()`d as
   soon as they're no longer needed; the long-lived license DEK cache is
   `mlock()`ed while resident and cleansed at `MSHUTDOWN`; `RLIMIT_CORE` is
   set to 0 at `MINIT` so a crash mid-decrypt can't drop key material or
   decrypted op_array contents into a core file. All best-effort and POSIX-only
   (compiled out on Windows) -- see `opdump_disable_core_dumps()`/
   `opdump_lock_mem()` in `php/src/opdump.c`.

## Deliberately Not Done

- **Loader `.so` self-checksum.** Would need a two-pass build (build, hash,
  patch the expected hash in, rebuild) wired into the build scripts. Real
  value, but a packaging-track change, not a container-format one.
- **Anti-ptrace/anti-debug.** Platform-specific, fragile, and trivially
  bypassed by anyone who already has the key unwrapped in memory -- low
  value for the complexity and risk of breaking legitimate debugging.
- **Pre-encryption opcode reordering/dummy-opcode injection** (sometimes
  bundled under "obfuscation"). Jump targets, `try_catch_array`, and live
  ranges are all opline-index/range-based; getting this wrong reproduces the
  exact class of silent-Reflection-corruption bug the README calls out as
  disqualifying for a commercial encoder. Variable-name obfuscation was
  implemented instead (`opdump_op_array_uses_dynamic_vars()`,
  `bytecode-dump --obfuscate`) because CVs are referenced by index, not
  name, making a rename a pure metadata edit with no control-flow risk.

## Open Questions

- What exact PHP ABI/build identifier should be stored beyond `PHP_VERSION_ID`?
- How should OPcache participation interact with license expiry or key rotation?
- Which diagnostics are safe to expose without helping an attacker distinguish
  wrong-key, wrong-version, and tamper cases?
- License expiry/revocation policy isn't modeled yet -- `bytecode.license.json`
  only carries a wrapped DEK, no expiry or revocation identifier.
