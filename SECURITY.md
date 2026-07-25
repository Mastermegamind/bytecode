# Security Policy

This project handles encryption keys, RSA key wrapping, and native Zend
Engine internals — please report security issues privately rather than
through a public GitHub issue.

## Reporting a Vulnerability

Email **hello@megamindtechnologies.com** with:

- A description of the issue and its impact.
- Steps to reproduce (a minimal PHP script + container/key setup is ideal).
- Which component is affected: the `opdump` C extension
  (`php/src/opdump.c`), the container format/key handling
  (`php/bin/bytecode-pack`, `php/bin/bytecode-dump`,
  `php/bin/bytecode-license-keygen`), or tooling elsewhere.

Please do not open a public issue or PR that discloses an unpatched
vulnerability. You'll get an acknowledgement within a few days and a plan
for a fix/disclosure timeline.

## Scope

In scope:

- Container format / cryptographic weaknesses (`BYTC1`/`BYTC2`, AES-256-GCM
  usage, HKDF key derivation, RSA-OAEP key wrapping in license mode).
- Memory-safety issues in the `opdump` C extension, especially in the
  untrusted-input paths (parsing `.bytc` containers, `bytecode.map`,
  `bytecode.license.json`).
- Cases where the loader executes reconstructed bytecode with different
  semantics than the original source (a correctness bug with security
  implications, not just a crash).

Out of scope:

- The project's current phase status is that only PHP 8.4 is fully
  supported (see `docs/PLAN.md`); issues specific to PHP 8.1-8.3/8.5 builds
  not yet working are tracked as normal bugs, not security reports.
- Denial-of-service via a self-inflicted misconfiguration (e.g. losing your
  own `BYTECODE_KEY` or license private key).
