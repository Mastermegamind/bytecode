/*
 * bytecode_tools.h — C ABI for libbytecode_tools (see rust/src/ffi.rs).
 *
 * Link against libbytecode_tools.so / .dylib / .dll (cdylib) or
 * libbytecode_tools.a (staticlib), built by `cargo build --release` in rust/.
 *
 * Lets a C / Dart (Flutter) / any-FFI caller run keygen / pack / verify
 * in-process — no CLI subprocess, no PHP runtime.
 *
 * Conventions
 *   - Input strings are NUL-terminated UTF-8 (const char*).
 *   - Output strings are heap-allocated by the library; free them with
 *     bt_string_free (NOT libc free).
 *   - int-returning calls: 0 = success, -1 = failure. On failure a message is
 *     available from bt_last_error() until the next call on the same thread.
 */
#ifndef BYTECODE_TOOLS_H
#define BYTECODE_TOOLS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Last error on this thread ("" if none). Owned by the library; copy it if you
 * need it past the next FFI call on this thread. */
const char *bt_last_error(void);

/* Free a string returned by this library (bt_keygen_hex, verify out_json).
 * NULL is a no-op. */
void bt_string_free(char *s);

/* Fresh 32-byte key as 64 lowercase hex chars. Returns a heap string (free with
 * bt_string_free) or NULL on allocation failure. */
char *bt_keygen_hex(void);

/* Wrap a raw OPD2 blob (raw_path) into a BYTC v2 container (out_path), stamping
 * php_version_id into the header. Key material comes from the environment
 * (BYTECODE_DEK/KEY/OPDUMP_KEY/VENDOR_KEY) or the vendor key file (created if
 * absent). Returns 0 on success, -1 on failure (see bt_last_error). */
int bt_pack(const char *raw_path, const char *out_path, uint32_t php_version_id);

/* Verify one .bytc container. decrypt_test != 0 also AES-GCM-decrypts it.
 * want_php_version_id is the PHP_VERSION_ID the container must declare.
 * On success, if out_json is non-NULL, *out_json receives a heap JSON summary
 *   {"backend":..,"backend_format":..,"sha256":..,"size":N,"payload_size":N|null}
 * (free with bt_string_free). Returns 0 on success, -1 on failure. */
int bt_verify_one(const char *path, int decrypt_test, uint32_t want_php_version_id,
                  char **out_json);

/* Best-effort local PHP_VERSION_ID: reads BYTECODE_PHP_VERSION_ID, else probes
 * a `php` on PATH. Returns the id, or -1 if undetermined. Never exits. */
int64_t bt_detect_php_version_id(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* BYTECODE_TOOLS_H */
