# Phase 4 Key And Integrity Model

This note captures constraints that should influence the Phase 1/2 container
work before production key handling is implemented.

## Current Development Model

- `BYTC1` containers are encrypted and authenticated with AES-256-GCM.
- The key is 32 bytes supplied as 64 hex characters through `BYTECODE_KEY` or
  `OPDUMP_KEY`.
- The loader must reject wrong keys or tampered ciphertext during GCM
  authentication before parsing the decrypted Zend payload.
- This is acceptable for local development and CI, but not for production
  licensing.

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

## Candidate Shape

1. Keep a small plaintext header with magic, container version, PHP version,
   build/ABI id, cipher id, key id, nonce, and authentication tag.
2. Authenticate and decrypt a payload that includes the serialized Zend
   structures plus any manifest fields required for execution.
3. Resolve the actual AES key through a pluggable key provider selected by key
   id. Development can keep using environment variables; production can later
   use a license file, local service, hardware-backed secret, or deployment
   secret store.
4. Treat licensing as a key-provider policy decision: if the license is invalid,
   no payload key is returned and the loader never reaches Zend reconstruction.

## Open Questions

- What exact PHP ABI/build identifier should be stored beyond `PHP_VERSION_ID`?
- Should `bytecode.map` be authenticated as a separate manifest, embedded into
  a signed bundle, or regenerated during installation?
- How should OPcache participation interact with license expiry or key rotation?
- Which diagnostics are safe to expose without helping an attacker distinguish
  wrong-key, wrong-version, and tamper cases?
