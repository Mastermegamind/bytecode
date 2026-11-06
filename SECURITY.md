# Security Policy

Bytecode PHP Loader handles encoded PHP bytecode, encrypted assets, vendor
keys, license keys, and machine authorization policies. Please report security
issues privately before publishing details.

## Threat Model

An encoder changes the trust boundary; it does not make source recovery
impossible. Being explicit about what this project does and does not defend
against keeps expectations honest and anchors future design decisions.

### What it protects against

- **Casual source recovery.** Distributed artifacts are compiled Zend
  bytecode inside authenticated, encrypted containers, not readable PHP source.
  Reading a `.bcenc` file does not yield the original source text.
- **Tampering by a party without the key.** Containers are AES-256-GCM
  encrypted with per-container HKDF-derived subkeys; the header is
  authenticated as AAD. The `bytecode.manifest.json`/`bytecode.map` wiring is
  covered by an HMAC, so a third party cannot repoint a source path at a
  different container, flip a byte, or forge a container without the key.
- **License tampering by the key holder — only when a vendor seal is used.**
  The Ed25519 vendor seal (`bytecode.seal.json`, verified against a public-key
  trust anchor compiled into the loader) is the one layer that binds the
  license constraints (expiry/domain/IP/machine/fingerprint) against the party
  actually running the code, because they hold the DEK-unwrapping private key
  but not the vendor signing key.
- **Malformed/hostile container structure crashing the loader.** The native
  deserializer bounds every input-derived count and length, fails closed on
  truncation, and caps recursion before allocating or reconstructing Zend
  structures.

### What it does NOT protect against (non-goals)

- **An attacker who has the decryption key.** Anyone holding `BYTECODE_KEY` /
  the unwrapped DEK can decrypt containers. Protect keys accordingly.
- **An attacker with the running loader plus a debugger.** Once the loader has
  decrypted and reconstructed op_arrays in memory, someone who can attach a
  debugger, patch the loader, or dump process memory on the host can recover
  bytecode-equivalent structures. The in-memory hardening (mlock, cleanse,
  core-dumps-off) raises the cost of accidental leakage; it is not a defense
  against a privileged local attacker. Anti-debug/anti-ptrace is a deliberate
  non-goal (fragile, trivially bypassed by someone who already has the above).
- **Perfect obfuscation.** Variable-name obfuscation removes CV names only.
  Control flow, string literals, and structure remain, by design — the project
  does not reorder opcodes or inject dummy ops (that risks silent Reflection
  corruption).
- **A compromised build host.** The encode-side tools (`bytecode-dump`,
  `bytecode-pack`) hold plaintext bytecode and key material in an ordinary PHP
  process. Run them only on a trusted machine.

## Key Handling

- **Environment-variable keys (`BYTECODE_KEY`, `OPDUMP_KEY`, `BYTECODE_DEK`)
  are for development and CI convenience only.** Process environment is
  readable via `/proc/<pid>/environ` by same-user processes, is inherited by
  child processes, and routinely leaks into shell history, logs, and crash
  reports. Do not use env-var keys for production distribution.
- **Prefer file- and license-based delivery for production:** the RSA-wrapped
  DEK in `bytecode.license.json` (`OPDUMP_LICENSE_KEY_FILE`), the vendor key
  file (`BYTECODE_VENDOR_KEY_FILE`), or a compiled-in vendor public-key anchor.
  These keep the long-lived secret off the command line and out of the
  environment of unrelated child processes.
- Treat throwaway keys as disposable and never commit real keys; `.gitignore`
  already excludes `*.key`, `*.pem`, and encoded blobs.

## Reporting A Vulnerability

Email hello@megamindtechnologies.com or open a private security advisory in the
project host if that feature is available. Include:

- A short description of the issue.
- Reproduction steps or proof of concept.
- Affected version, commit, platform, and PHP version.
- Any known impact on encoded packages, keys, or runtime authorization.

Please do not include real customer keys or production encoded applications in
reports. Use throwaway keys and minimal test projects.

## Supported Versions

The project is early-stage. Security fixes are expected to target the latest
public source version unless a separate commercial support agreement says
otherwise.
