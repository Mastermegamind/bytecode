# Bytecode container & crypto format

Authoritative description of the on-disk formats and cryptographic scheme shared
by the C loader (`php/src/opdump.c`), the PHP tools (`php/bin/*`) and the Rust
tools (`rust/`). The PHP and Rust implementations are byte-for-byte compatible;
this document is the contract that keeps them so. It is enforced by
`php/tests/run-rust-parity.sh` and the `rust-tools` CI workflow.

All integers are **little-endian**. Notation:

- `u32` / `u64` — unsigned LE integer of that width.
- `lp32<bytes>` — a `u32` length followed by exactly that many bytes.
- `lp64<bytes>` — a `u64` length followed by exactly that many bytes.

## BYTC container

A single encoded unit (one PHP file's bytecode, or one asset). Produced by
`bytecode-pack` / `bytecode-pack-asset`; consumed by the loader and
`bytecode-verify`.

```
offset  field           encoding   notes
------  --------------  ---------  -------------------------------------------
0       magic           "BYTC"     4 ASCII bytes
4       version         u32        1 or 2 (writers always emit 2)
8       php_version_id  u32        PHP_VERSION_ID the payload targets
        backend_id      lp32       "php-zend-opdump" | "bytecode-asset"
        backend_format  lp32       "OPD2" (php) | "RAW1" (asset)
  --- version 2 only ---
        cipher_id       u32        1 = AES-256-GCM
        key_id          lp32       16 random bytes (per-file HKDF salt)
  --- end of header; header_size = current offset ---
        nonce           lp32       12 bytes (GCM nonce)
        tag             lp32       16 bytes (GCM authentication tag)
        ciphertext      lp64       the AEAD ciphertext
(EOF)                              no trailing bytes are permitted
```

The **header** — every byte from offset 0 up to `header_size` (i.e. through
`key_id`) — is used verbatim as the AEAD associated data (AAD), so any change to
the version, target PHP version, backend, cipher or key id invalidates the tag.

`bytecode-verify` additionally checks: `php_version_id` equals the running
PHP's, `backend_id`/`backend_format` is a known pair, `cipher_id == 1`,
`nonce` is 12 bytes and `tag` is 16 bytes.

## Key derivation & payload encryption (version 2)

Input keying material (IKM) is 32 bytes, supplied as 64 hex characters. For
**encryption / decrypt-test** it is resolved in order:

```
BYTECODE_DEK > BYTECODE_KEY > OPDUMP_KEY > BYTECODE_VENDOR_KEY > build/vendor-secret.key
```

Per container:

```
key = HKDF-SHA256(ikm = IKM, salt = key_id, info = "bytecode-v2", L = 32)
ciphertext, tag = AES-256-GCM(key, nonce = nonce, aad = header[0..header_size], plaintext = payload)
```

For `php-zend-opdump` the plaintext begins with the 4 bytes `OPD2`; for
`bytecode-asset` it is the raw asset bytes. Decryption reverses this; a GCM
authentication failure means wrong key or tampering and is rejected.

HKDF here is RFC 5869. Where a **salt is empty** (see manifest auth below), the
RFC's "no salt" case applies — a salt of `HashLen` zero bytes — which is what
PHP's `hash_hkdf(..., salt: '')` and the Rust `Hkdf::new(None, ..)` both do.

## Manifest, map & signature

`bytecode-dump` writes, in the output directory:

- `bytecode.manifest.json` — `{ "format":"bytecode-manifest-v1", "map":"bytecode.map", "files":[ {source, output, php_version_id, backend, container|backend_format, sha256, size} ] }`.
- `bytecode.map` — newline-separated `"<source-abs>\t<output-relative>"` lines; `#`-prefixed and blank lines ignored.
- `bytecode.manifest.sig` — a 64-hex HMAC:

```
sig_key = HKDF-SHA256(ikm = IKM_manifest, salt = "", info = "bytecode-manifest-auth", L = 32)
sig     = hex( HMAC-SHA256(sig_key, manifest_bytes || 0x00 || map_bytes) )
```

`IKM_manifest` is resolved **without** `BYTECODE_DEK`:
`BYTECODE_KEY > OPDUMP_KEY > BYTECODE_VENDOR_KEY > build/vendor-secret.key`.
When no IKM is available, `bytecode-verify` performs only a hex-shape check on
the signature.

## Vendor seal (optional)

`bytecode.seal.json` — `{ "format":"bytecode-seal-v1", "alg":"Ed25519",
"manifest_sha256", "map_sha256", "license_sha256"?, "policy_sha256"?,
"signature" }`. The signature is base64 of an Ed25519 signature (one-shot, no
prehash) over the exact message:

```
bytecode-seal-v1\n
manifest:<sha256 hex of bytecode.manifest.json>\n
map:<sha256 hex of bytecode.map>\n
license:<sha256 hex of bytecode.license.json, or "-">\n
policy:<policy_sha256>\n          # this line only when policy_sha256 is present
```

`policy_sha256`, when present, is `sha256(policy_message)` where
`policy_message` is:

```
bytecode-seal-policy-v1\n
machine_id:<value or "-">\n
domains:<value or "-">\n
ips:<value or "-">\n
hostnames:<value or "-">\n
fingerprints:<value or "-">\n
expires_at:<value or "-">\n
license_id:<value or "-">\n
activation_token:<value or "-">\n
revocation_url:<value or "-">\n
unlock_policy:<value or "-">\n
```

`bytecode-verify` always checks the seal's digests; it checks the Ed25519
signature when a vendor public key PEM is given via `BYTECODE_VENDOR_PUBKEY` /
`OPDUMP_VENDOR_PUBKEY_FILE` (the anchor the loader trusts).

## Package manifest

`bytecode-package-sign` writes `bytecode.package.json` —
`{ "format":"bytecode-package-v1", "tool_version", "created_at", "file_count",
"files":[ {path, sha256, size} ], "sha256" }`.

- `files` covers every file in the output directory except `bytecode.package.json`
  itself, sorted ascending by `path` (byte order).
- `created_at` is ISO-8601 UTC (`gmdate('c')`).
- The top-level `sha256` is `sha256( compact-json(files) )`, where `compact-json`
  is PHP's `json_encode($files, JSON_UNESCAPED_SLASHES)` (no spaces, `/`
  unescaped, non-ASCII as `\uXXXX`).
- The file itself is pretty-printed with 4-space indentation (`JSON_PRETTY_PRINT
  | JSON_UNESCAPED_SLASHES`) and a trailing newline.
