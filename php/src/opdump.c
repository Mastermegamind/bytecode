/*
 * opdump - Phase 0 feasibility spike.
 *
 * Hooks zend_compile_file so that, in "dump" mode, a normally-compiled
 * top-level zend_op_array is serialized to disk after compilation, and in
 * "load" mode a previously-dumped op_array is reconstructed from disk
 * *without* the engine ever parsing PHP source text for that request.
 *
 * Scope (see docs/PHASE0.md in the repo root): top-level op_arrays plus
 * dynamically-declared functions, literal types NULL/BOOL/LONG/DOUBLE/STRING,
 * CV variable names, and simple scalar arg_info. Classes, try/catch, and static
 * variables are still later rungs.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "zend_compile.h"
#ifndef ZEND_EXT_API
# define ZEND_EXT_API ZEND_DLEXPORT
#endif
#include "zend_extensions.h"
#include "zend_vm.h"
#include "zend_string.h"
#include "zend_inheritance.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/crypto.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
# include <direct.h>
# include <windows.h>
# define strcasecmp _stricmp
# define strncasecmp _strnicmp
# ifndef PATH_MAX
#  define PATH_MAX MAX_PATH
# endif
# define realpath(path, resolved) _fullpath((resolved), (path), PATH_MAX)
#else
# include <sys/resource.h>
# include <sys/mman.h>
# include <unistd.h>
# include <strings.h>
#endif

#ifndef OPDUMP_VENDOR_SECRET_HEX
# define OPDUMP_VENDOR_SECRET_HEX ""
#endif

#ifndef OPDUMP_VENDOR_PUBKEY_HEX
# define OPDUMP_VENDOR_PUBKEY_HEX ""
#endif

#ifndef OPDUMP_VERSION
# define OPDUMP_VERSION "0.0.1phase2"
#endif

#ifndef ZEND_HASH_MAP_FOREACH_PTR
# define ZEND_HASH_MAP_FOREACH_PTR ZEND_HASH_FOREACH_PTR
#endif
#ifndef ZEND_HASH_MAP_FOREACH_STR_KEY_PTR
# define ZEND_HASH_MAP_FOREACH_STR_KEY_PTR ZEND_HASH_FOREACH_STR_KEY_PTR
#endif

#ifndef ZEND_TYPE_HAS_LITERAL_NAME
# define ZEND_TYPE_HAS_LITERAL_NAME(t) (0)
#endif
#ifndef _ZEND_TYPE_LITERAL_NAME_BIT
# define _ZEND_TYPE_LITERAL_NAME_BIT 0u
#endif

/* Best-effort in-memory hardening for key material (Rung D). mlock/munlock
 * and RLIMIT_CORE are POSIX-only; Windows builds compile these out entirely
 * rather than partially-lock memory in a way nothing here relies on. */
static void opdump_disable_core_dumps(void)
{
#ifndef _WIN32
    struct rlimit rl = {0, 0};
    /* Best-effort: an unprivileged process may not be able to raise this
     * back up, which is exactly the point -- it only ever tightens. */
    setrlimit(RLIMIT_CORE, &rl);
#endif
}

static void opdump_lock_mem(void *p, size_t len)
{
#ifndef _WIN32
    /* Best-effort: no CAP_IPC_LOCK/RLIMIT_MEMLOCK is common under containers.
     * A failure here only means this buffer isn't pinned against swap, not
     * that key handling is broken. */
    mlock(p, len);
#else
    (void) p; (void) len;
#endif
}

static void opdump_unlock_mem(void *p, size_t len)
{
#ifndef _WIN32
    munlock(p, len);
#else
    (void) p; (void) len;
#endif
}

static zend_op_array *(*opdump_orig_compile_file)(zend_file_handle *file_handle, int type);
static bool opdump_hooks_started = false;

static void opdump_init_run_time_cache(zend_op_array *op_array)
{
    if (op_array->cache_size <= 0) {
        return;
    }
    void **run_time_cache = (void **) ecalloc(1, (size_t) op_array->cache_size);
#if PHP_VERSION_ID < 80200
    void ***run_time_cache_ptr = (void ***) emalloc(sizeof(void **));
    *run_time_cache_ptr = run_time_cache;
    ZEND_MAP_PTR_INIT(op_array->run_time_cache, run_time_cache_ptr);
#else
    ZEND_MAP_PTR_INIT(op_array->run_time_cache, run_time_cache);
#endif
#ifdef ZEND_ACC_HEAP_RT_CACHE
    op_array->fn_flags |= ZEND_ACC_HEAP_RT_CACHE;
#endif
}

static void opdump_bind_fcall_caches(zend_op_array *op_array)
{
    if (op_array->cache_size <= 0) {
        return;
    }

    void **run_time_cache = RUN_TIME_CACHE(op_array);
    if (!run_time_cache) {
        return;
    }

    for (uint32_t i = 0; i < op_array->last; i++) {
        zend_op *opline = &op_array->opcodes[i];
        if (opline->opcode != ZEND_INIT_FCALL || opline->op2_type != IS_CONST) {
            continue;
        }
        if (opline->result.num >= (uint32_t) op_array->cache_size) {
            continue;
        }

        zval *fname = RT_CONSTANT(opline, opline->op2);
        if (Z_TYPE_P(fname) != IS_STRING) {
            continue;
        }

        zval *func_zv = zend_hash_find(EG(function_table), Z_STR_P(fname));
        if (!func_zv) {
            if (getenv("OPDUMP_DEBUG")) {
                fprintf(stderr, "[opdump debug] fcall cache miss: %s\n", Z_STRVAL_P(fname));
            }
            continue;
        }

        zend_function *fbc = Z_FUNC_P(func_zv);
        *(zend_function **)((char *) run_time_cache + opline->result.num) = fbc;
        if (getenv("OPDUMP_DEBUG")) {
            fprintf(stderr, "[opdump debug] fcall cache bind: %s offset=%u fbc=%p slot=%p\n",
                Z_STRVAL_P(fname), opline->result.num, (void*) fbc,
                (void*)((char *) run_time_cache + opline->result.num));
        }
    }

    for (uint32_t i = 0; i < op_array->num_dynamic_func_defs; i++) {
        opdump_bind_fcall_caches(op_array->dynamic_func_defs[i]);
    }
}

typedef struct _opdump_map_entry {
    char *source;
    char *encoded;
    struct _opdump_map_entry *next;
} opdump_map_entry;

static opdump_map_entry *opdump_tree_map = NULL;

#ifdef _WIN32
static FILE *opdump_fmemopen(const void *buf, size_t len, const char *mode)
{
    (void) mode;
    FILE *f = tmpfile();
    if (!f) {
        return NULL;
    }
    if (len && fwrite(buf, 1, len, f) != len) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    return f;
}
# define fmemopen(buf, len, mode) opdump_fmemopen((buf), (len), (mode))
#endif

/* ---- binary format helpers ---- */

#define OPDUMP_MAGIC "OPD2"
#define OPDUMP_MAX_STRING_LEN (16u * 1024u * 1024u)
#define OPDUMP_MAX_OPCODES 1000000u
#define OPDUMP_MAX_LITERALS 1000000u
#define OPDUMP_MAX_VARS 200000u
#define OPDUMP_MAX_ARGS 10000u
#define OPDUMP_MAX_DYNAMIC_FUNCS 100000u
#define OPDUMP_MAX_FILE_FUNCTIONS 100000u
#define OPDUMP_MAX_FILE_CLASSES 100000u
#define OPDUMP_MAX_CLASS_METHODS 10000u
#define OPDUMP_MAX_CLASS_PROPERTIES 10000u
#define OPDUMP_MAX_TRY_CATCH 100000u

typedef enum {
    OPDUMP_LIT_NULL  = 0,
    OPDUMP_LIT_TRUE  = 1,
    OPDUMP_LIT_FALSE = 2,
    OPDUMP_LIT_LONG  = 3,
    OPDUMP_LIT_DOUBLE = 4,
    OPDUMP_LIT_STRING = 5,
    OPDUMP_LIT_UNDEF = 6,
    OPDUMP_LIT_ARRAY = 7,
    OPDUMP_LIT_AST = 8
} opdump_lit_tag;

static void w_u8(FILE *f, uint8_t v)   { fwrite(&v, 1, 1, f); }
static void w_u32(FILE *f, uint32_t v) { fwrite(&v, sizeof(v), 1, f); }
static void w_u64(FILE *f, uint64_t v) { fwrite(&v, sizeof(v), 1, f); }
static void w_i32(FILE *f, int32_t v)  { fwrite(&v, sizeof(v), 1, f); }
static void w_dbl(FILE *f, double v)   { fwrite(&v, sizeof(v), 1, f); }
static void w_str(FILE *f, const char *s, size_t len) {
    w_u32(f, (uint32_t)len);
    if (len) fwrite(s, 1, len, f);
}
static void w_zstr(FILE *f, zend_string *s) {
    if (s) {
        w_u8(f, 1);
        w_str(f, ZSTR_VAL(s), ZSTR_LEN(s));
    } else {
        w_u8(f, 0);
    }
}

static void r_exact(FILE *f, void *buf, size_t len, const char *field)
{
    if (len == 0) {
        return;
    }
    if (fread(buf, 1, len, f) != len) {
        php_error_docref(NULL, E_ERROR, "opdump: truncated or malformed blob while reading %s", field);
    }
}

static uint8_t  r_u8(FILE *f)  { uint8_t v = 0; r_exact(f, &v, sizeof(v), "u8"); return v; }
static uint32_t r_u32(FILE *f) { uint32_t v = 0; r_exact(f, &v, sizeof(v), "u32"); return v; }
static uint64_t r_u64(FILE *f) { uint64_t v = 0; r_exact(f, &v, sizeof(v), "u64"); return v; }
static int32_t  r_i32(FILE *f) { int32_t v = 0; r_exact(f, &v, sizeof(v), "i32"); return v; }
static double   r_dbl(FILE *f) { double v = 0; r_exact(f, &v, sizeof(v), "double"); return v; }

static void opdump_guard_u32(uint32_t value, uint32_t max, const char *field)
{
    if (value > max) {
        php_error_docref(NULL, E_ERROR, "opdump: %s count %u exceeds limit %u", field, value, max);
    }
}

static void opdump_guard_i32_nonnegative(int32_t value, uint32_t max, const char *field)
{
    if (value < 0 || (uint32_t)value > max) {
        php_error_docref(NULL, E_ERROR, "opdump: %s count %d is outside supported range 0..%u", field, value, max);
    }
}

/* caller frees */
static char *r_str(FILE *f, uint32_t *out_len) {
    uint32_t len = r_u32(f);
    if (len > OPDUMP_MAX_STRING_LEN) {
        php_error_docref(NULL, E_ERROR, "opdump: string length %u exceeds limit %u", len, OPDUMP_MAX_STRING_LEN);
    }
    char *buf = malloc(len + 1);
    if (!buf) {
        php_error_docref(NULL, E_ERROR, "opdump: failed to allocate %u byte string", len);
    }
    r_exact(f, buf, len, "string payload");
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}
static zend_string *r_zstr(FILE *f, bool intern)
{
    if (!r_u8(f)) {
        return NULL;
    }
    uint32_t len;
    char *s = r_str(f, &len);
    zend_string *zs = zend_string_init(s, len, 0);
    free(s);
    if (intern) {
        zs = zend_new_interned_string(zs);
    }
    zend_string_hash_val(zs);
    return zs;
}

static void opdump_write_op_array(FILE *f, zend_op_array *op_array);
static zend_op_array *opdump_read_op_array(FILE *f);
static void opdump_write_class_entry(FILE *f, zend_class_entry *ce);
static zend_class_entry *opdump_read_class_entry(FILE *f);

/* Only the scalar-mask path is needed for rung 2. Named class/list types are
 * serialized by shape so later rungs fail explicitly rather than smuggling
 * process-local pointers into the blob. */
static void opdump_write_type(FILE *f, zend_type type)
{
    w_u32(f, type.type_mask);
    if (ZEND_TYPE_HAS_NAME(type)) {
        w_u8(f, 1);
        w_zstr(f, ZEND_TYPE_NAME(type));
    } else if (ZEND_TYPE_HAS_LITERAL_NAME(type)) {
        const char *name = ZEND_TYPE_LITERAL_NAME(type);
        w_u8(f, 2);
        w_str(f, name, strlen(name));
    } else if (ZEND_TYPE_HAS_LIST(type)) {
        php_error_docref(NULL, E_WARNING, "opdump: union/intersection types are not supported yet");
        w_u8(f, 0);
    } else {
        w_u8(f, 0);
    }
}

static zend_type opdump_read_type(FILE *f)
{
    zend_type type = ZEND_TYPE_INIT_NONE(0);
    uint32_t mask = r_u32(f);
    uint8_t ptr_kind = r_u8(f);

    if (ptr_kind == 1) {
        zend_string *name = r_zstr(f, true);
        type = (zend_type) ZEND_TYPE_INIT_PTR_MASK(name, mask);
    } else if (ptr_kind == 2) {
        uint32_t len;
        char *name = r_str(f, &len);
        zend_string *zs = zend_string_init(name, len, 0);
        free(name);
        zs = zend_new_interned_string(zs);
        type = (zend_type) ZEND_TYPE_INIT_PTR_MASK(zs, (mask & ~_ZEND_TYPE_LITERAL_NAME_BIT) | _ZEND_TYPE_NAME_BIT);
    } else {
        type = (zend_type) ZEND_TYPE_INIT_MASK(mask & ~_ZEND_TYPE_KIND_MASK);
    }
    return type;
}

static void opdump_write_arg_info(FILE *f, zend_arg_info *arg_info)
{
    w_zstr(f, arg_info->name);
    opdump_write_type(f, arg_info->type);
    /* PHP 8.4 user-function defaults are represented by RECV_INIT opcodes and
     * literals. arg_info.default_value is not consistently a live zend_string*
     * at this point, so do not persist it until the reflection rung proves the
     * exact representation needed. */
    w_zstr(f, NULL);
}

static void opdump_read_arg_info(FILE *f, zend_arg_info *arg_info)
{
    arg_info->name = r_zstr(f, true);
    arg_info->type = opdump_read_type(f);
    arg_info->default_value = r_zstr(f, true);
}

static int opdump_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool opdump_hex_decode(const char *hex, unsigned char *out, size_t out_len)
{
    if (strlen(hex) != out_len * 2) {
        return false;
    }
    for (size_t i = 0; i < out_len; i++) {
        int hi = opdump_hex_nibble(hex[i * 2]);
        int lo = opdump_hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return true;
}

static bool opdump_key_from_env(unsigned char key[32])
{
    const char *hex = getenv("BYTECODE_KEY");
    if (!hex || !hex[0]) {
        hex = getenv("OPDUMP_KEY");
    }
    if (!hex || strlen(hex) != 64) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        int hi = opdump_hex_nibble(hex[i * 2]);
        int lo = opdump_hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        key[i] = (unsigned char)((hi << 4) | lo);
    }
    return true;
}

static bool opdump_vendor_key(unsigned char key[32])
{
    const char *hex = getenv("BYTECODE_VENDOR_KEY");
    if (hex && hex[0]) {
        return strlen(hex) == 64 && opdump_hex_decode(hex, key, 32);
    }

    const char *file = getenv("BYTECODE_VENDOR_KEY_FILE");
    if (file && file[0]) {
        FILE *f = fopen(file, "rb");
        if (!f) {
            return false;
        }
        char buf[80];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ' || buf[n - 1] == '\t')) {
            buf[--n] = '\0';
        }
        return n == 64 && opdump_hex_decode(buf, key, 32);
    }

    hex = OPDUMP_VENDOR_SECRET_HEX;
    if (!hex || !hex[0] || strlen(hex) != 64) {
        return false;
    }

    return opdump_hex_decode(hex, key, 32);
}

/* RFC 5869 HKDF-SHA256, fixed 32-byte output (== one HMAC-SHA256 block, so
 * Expand needs exactly T(1); no need for the general multi-block loop). Must
 * match PHP's hash_hkdf('sha256', $ikm, 32, $info, $salt) bit for bit --
 * that's how bytecode-pack derives the same per-container key on the write
 * side without duplicating this in C. */
static bool opdump_hkdf_sha256(
    const unsigned char *ikm, size_t ikm_len,
    const unsigned char *salt, size_t salt_len,
    const char *info, unsigned char out[32]
) {
    unsigned char prk[32];
    unsigned int prk_len = 0;
    if (!HMAC(EVP_sha256(), salt, (int)salt_len, ikm, ikm_len, prk, &prk_len) || prk_len != 32) {
        return false;
    }

    size_t info_len = strlen(info);
    unsigned char t[256];
    if (info_len + 1 > sizeof(t)) {
        OPENSSL_cleanse(prk, sizeof(prk));
        return false;
    }
    memcpy(t, info, info_len);
    t[info_len] = 0x01;

    unsigned int out_len = 0;
    bool ok = HMAC(EVP_sha256(), prk, (int)sizeof(prk), t, info_len + 1, out, &out_len) != NULL
        && out_len == 32;
    OPENSSL_cleanse(prk, sizeof(prk));
    OPENSSL_cleanse(t, sizeof(t));
    return ok;
}

/* Defined further down (needs the path-joining helpers below it). */
static bool opdump_license_resolve_ikm(const char *key_file, unsigned char ikm[32]);
static const char *opdump_current_container_path = NULL;

/* Resolves the IKM (input keying material) used to derive BYTC2+ per-container
 * keys. If OPDUMP_LICENSE_KEY_FILE is set, license mode is what the caller
 * intended, so a failure there is reported and NOT silently downgraded to
 * the shared-secret path -- a misconfigured license should fail loudly, not
 * quietly fall back to a dev key. BYTC1 containers never call this -- they
 * use opdump_key_from_env()'s output directly as the AES key, unchanged, so
 * existing v1 containers keep decrypting exactly as before. */
static bool opdump_resolve_ikm(unsigned char ikm[32])
{
    const char *key_file = getenv("OPDUMP_LICENSE_KEY_FILE");
    if (key_file && key_file[0]) {
        return opdump_license_resolve_ikm(key_file, ikm);
    }
    if (opdump_key_from_env(ikm)) {
        return true;
    }
    return opdump_vendor_key(ikm);
}

static uint32_t opdump_le32(const unsigned char *p)
{
    return ((uint32_t)p[0])
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static uint64_t opdump_le64(const unsigned char *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

static bool opdump_take_bytes(const unsigned char *buf, size_t len, size_t *off, size_t want, const unsigned char **out)
{
    if (*off > len || want > len - *off) {
        return false;
    }
    *out = buf + *off;
    *off += want;
    return true;
}

static bool opdump_take_u32(const unsigned char *buf, size_t len, size_t *off, uint32_t *out)
{
    const unsigned char *p;
    if (!opdump_take_bytes(buf, len, off, 4, &p)) {
        return false;
    }
    *out = opdump_le32(p);
    return true;
}

static bool opdump_take_u64(const unsigned char *buf, size_t len, size_t *off, uint64_t *out)
{
    const unsigned char *p;
    if (!opdump_take_bytes(buf, len, off, 8, &p)) {
        return false;
    }
    *out = opdump_le64(p);
    return true;
}

static unsigned char *opdump_read_file_bytes(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        php_error_docref(NULL, E_ERROR, "opdump: cannot open %s for reading", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long end = ftell(f);
    if (end < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    unsigned char *buf = (unsigned char *) malloc((size_t)end ? (size_t)end : 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)end, f);
    fclose(f);
    if (n != (size_t)end) {
        free(buf);
        return NULL;
    }
    *out_len = n;
    return buf;
}

static char *opdump_strdup_range(const char *start, size_t len)
{
    char *out = (char *) malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static bool opdump_is_path_separator(char c)
{
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

static bool opdump_is_absolute_path(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }
#ifdef _WIN32
    if ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) {
        return path[1] == ':' && opdump_is_path_separator(path[2]);
    }
    return opdump_is_path_separator(path[0]) && opdump_is_path_separator(path[1]);
#else
    return path[0] == '/';
#endif
}

static char *opdump_dirname_dup(const char *path)
{
    const char *slash = NULL;
    for (const char *p = path; *p; p++) {
        if (opdump_is_path_separator(*p)) {
            slash = p;
        }
    }
    if (!slash) {
        return strdup(".");
    }
    if (slash == path) {
        return opdump_strdup_range(path, 1);
    }
    return opdump_strdup_range(path, (size_t)(slash - path));
}

static char *opdump_join_path(const char *base, const char *path)
{
    if (!path || opdump_is_absolute_path(path)) {
        return path ? strdup(path) : NULL;
    }
    size_t base_len = strlen(base);
    size_t path_len = strlen(path);
    bool need_slash = base_len > 0 && !opdump_is_path_separator(base[base_len - 1]);
    char *out = (char *) malloc(base_len + (need_slash ? 1 : 0) + path_len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, base, base_len);
    if (need_slash) {
#ifdef _WIN32
        out[base_len++] = '\\';
#else
        out[base_len++] = '/';
#endif
    }
    memcpy(out + base_len, path, path_len);
    out[base_len + path_len] = '\0';
    return out;
}

static void opdump_map_add(char *source, char *encoded)
{
    opdump_map_entry *entry = (opdump_map_entry *) malloc(sizeof(opdump_map_entry));
    if (!entry) {
        free(source);
        free(encoded);
        return;
    }
    entry->source = source;
    entry->encoded = encoded;
    entry->next = opdump_tree_map;
    opdump_tree_map = entry;
}

/* Defined further down (needs opdump_read_text_file). */
static bool opdump_verify_manifest_signature(const char *map_dir, const char *map_path);
static bool opdump_policy_check_machine_id(const char *json, const char *label);
/* Vendor Ed25519 seal over manifest+map(+license). Returns true when the seal
 * verifies OR when no vendor public-key trust anchor is configured (legacy,
 * unenforced). Returns false only when an anchor IS configured and the seal is
 * missing/tampered/unusable -- i.e. fail closed. Defined near the manifest
 * signature check below (shares its file/crypto helpers). */
static bool opdump_verify_vendor_seal(const char *dir);

static bool opdump_load_tree_map(const char *path)
{
    char *map_dir = opdump_dirname_dup(path);
    if (!opdump_verify_manifest_signature(map_dir, path)) {
        free(map_dir);
        return false;
    }
    if (!opdump_verify_vendor_seal(map_dir)) {
        free(map_dir);
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        php_error_docref(NULL, E_WARNING, "opdump: cannot open OPDUMP_MAP %s", path);
        free(map_dir);
        return false;
    }

    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        char *newline = strpbrk(line, "\r\n");
        if (newline) {
            *newline = '\0';
        }
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        char *tab = strchr(line, '\t');
        if (!tab) {
            php_error_docref(NULL, E_WARNING, "opdump: malformed map line in %s", path);
            continue;
        }
        *tab = '\0';
        char *source = strdup(line);
        char *encoded = opdump_join_path(map_dir, tab + 1);
        if (source && encoded) {
            opdump_map_add(source, encoded);
        } else {
            free(source);
            free(encoded);
        }
    }
    free(map_dir);
    fclose(f);
    return true;
}

static const char *opdump_requested_filename(zend_file_handle *file_handle)
{
    if (file_handle->opened_path) {
        return ZSTR_VAL(file_handle->opened_path);
    }
    if (file_handle->filename) {
        return ZSTR_VAL(file_handle->filename);
    }
    return NULL;
}

static const char *opdump_find_tree_blob(zend_file_handle *file_handle)
{
    const char *filename = opdump_requested_filename(file_handle);
    char resolved[PATH_MAX];
    const char *real = NULL;

    if (!filename) {
        return NULL;
    }
    if (realpath(filename, resolved)) {
        real = resolved;
    }

    for (opdump_map_entry *entry = opdump_tree_map; entry; entry = entry->next) {
        if (strcmp(entry->source, filename) == 0 || (real && strcmp(entry->source, real) == 0)) {
            return entry->encoded;
        }
    }
    return NULL;
}

/* ---- license-mode key unwrap (Rung B) ---- */

static char *opdump_read_text_file(const char *path)
{
    size_t len = 0;
    unsigned char *raw = opdump_read_file_bytes(path, &len);
    if (!raw) {
        return NULL;
    }
    char *text = (char *) malloc(len + 1);
    if (!text) {
        free(raw);
        return NULL;
    }
    memcpy(text, raw, len);
    text[len] = '\0';
    free(raw);
    return text;
}

/* Deliberately narrow: bytecode.license.json is a flat, self-generated
 * object of quoted-string values (base64/hex/fixed literals only, written
 * with JSON_UNESCAPED_SLASHES), so a full JSON parser would be pure
 * unused-generality. Same spirit as bytecode.map's hand-rolled tab format. */
static char *opdump_json_extract_string(const char *json, const char *key)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) {
        return NULL;
    }
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') {
        return NULL;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') {
        return NULL;
    }
    p++;
    const char *start = p;
    while (*p && *p != '"') p++;
    if (*p != '"') {
        return NULL;
    }
    size_t val_len = (size_t)(p - start);
    char *out = (char *) malloc(val_len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, start, val_len);
    out[val_len] = '\0';
    return out;
}

static bool opdump_csv_match_one(const char *pattern, size_t pattern_len, const char *value)
{
    while (pattern_len > 0 && (*pattern == ' ' || *pattern == '\t')) {
        pattern++;
        pattern_len--;
    }
    while (pattern_len > 0 && (pattern[pattern_len - 1] == ' ' || pattern[pattern_len - 1] == '\t')) {
        pattern_len--;
    }
    if (pattern_len == 0 || !value || !value[0]) {
        return false;
    }
    if (pattern_len == 1 && pattern[0] == '*') {
        return true;
    }
    if (pattern_len > 2 && pattern[0] == '*' && pattern[1] == '.') {
        const char *suffix = pattern + 1;
        size_t suffix_len = pattern_len - 1;
        size_t value_len = strlen(value);
        return value_len >= suffix_len && strcasecmp(value + value_len - suffix_len, suffix) == 0;
    }
    return strlen(value) == pattern_len && strncasecmp(pattern, value, pattern_len) == 0;
}

static bool opdump_csv_contains(const char *csv, const char *value)
{
    if (!csv || !csv[0]) {
        return true;
    }
    const char *start = csv;
    for (const char *p = csv;; p++) {
        if (*p == ',' || *p == '\0') {
            if (opdump_csv_match_one(start, (size_t)(p - start), value)) {
                return true;
            }
            if (*p == '\0') {
                return false;
            }
            start = p + 1;
        }
    }
}

static bool opdump_license_check_csv(const char *json, const char *key, const char *actual, const char *label)
{
    char *allowed = opdump_json_extract_string(json, key);
    if (!allowed || !allowed[0]) {
        free(allowed);
        return true;
    }
    bool ok = opdump_csv_contains(allowed, actual);
    if (!ok) {
        php_error_docref(NULL, E_WARNING, "opdump: license %s does not allow this server", label);
    }
    free(allowed);
    return ok;
}

static bool opdump_license_check_expiry(const char *json)
{
    char *expires = opdump_json_extract_string(json, "expires_at");
    if (!expires || !expires[0]) {
        free(expires);
        return true;
    }
    time_t now = time(NULL);
    struct tm tm_buf;
#ifdef _WIN32
    struct tm *tm_utc = gmtime(&now);
    if (!tm_utc) {
        free(expires);
        return false;
    }
    tm_buf = *tm_utc;
#else
    if (!gmtime_r(&now, &tm_buf)) {
        free(expires);
        return false;
    }
#endif
    char today[11];
    strftime(today, sizeof(today), "%Y-%m-%d", &tm_buf);
    bool ok = strlen(expires) == 10 && strcmp(today, expires) <= 0;
    if (!ok) {
        php_error_docref(NULL, E_WARNING, "opdump: license expired");
    }
    free(expires);
    return ok;
}

static bool opdump_license_check_revocation(const char *json)
{
    char *license_id = opdump_json_extract_string(json, "license_id");
    const char *revocation_file = getenv("OPDUMP_REVOCATION_FILE");
    if (!license_id || !license_id[0] || !revocation_file || !revocation_file[0]) {
        free(license_id);
        return true;
    }
    char *revoked = opdump_read_text_file(revocation_file);
    if (!revoked) {
        free(license_id);
        return true;
    }
    bool is_revoked = false;
    char *line = revoked;
    while (line && *line) {
        char *next = strpbrk(line, "\r\n");
        if (next) {
            *next++ = '\0';
            while (*next == '\r' || *next == '\n') next++;
        }
        if (strcmp(line, license_id) == 0) {
            is_revoked = true;
            break;
        }
        line = next;
    }
    if (is_revoked) {
        php_error_docref(NULL, E_WARNING, "opdump: license_id is revoked");
    }
    free(revoked);
    free(license_id);
    return !is_revoked;
}

static bool opdump_license_check_activation(const char *json)
{
    char *required = opdump_json_extract_string(json, "activation_token");
    if (!required || !required[0]) {
        free(required);
        return true;
    }
    const char *actual = getenv("OPDUMP_ACTIVATION_TOKEN");
    bool ok = actual && strcmp(required, actual) == 0;
    if (!ok) {
        php_error_docref(NULL, E_WARNING, "opdump: activation token does not match");
    }
    free(required);
    return ok;
}

static bool opdump_license_check_policy(const char *json)
{
    char *policy = opdump_json_extract_string(json, "unlock_policy");
    if (!policy || !policy[0]) {
        free(policy);
        return true;
    }
    bool ok = true;
    if (strcmp(policy, "vendor+license") == 0 && !opdump_vendor_key((unsigned char[32]){0})) {
        php_error_docref(NULL, E_WARNING, "opdump: license requires a vendor-secret loader");
        ok = false;
    }
    free(policy);
    return ok;
}

static bool opdump_license_check_constraints(const char *json)
{
    const char *host = getenv("HTTP_HOST");
    if (!host || !host[0]) host = getenv("SERVER_NAME");
    const char *server_ip = getenv("SERVER_ADDR");
    const char *fingerprint = getenv("OPDUMP_MACHINE_FINGERPRINT");
    char hostname[256] = {0};
    gethostname(hostname, sizeof(hostname) - 1);

    return opdump_license_check_expiry(json)
        && opdump_license_check_csv(json, "domains", host, "domain")
        && opdump_license_check_csv(json, "ips", server_ip, "server IP")
        && opdump_license_check_csv(json, "hostnames", hostname, "hostname")
        && opdump_license_check_csv(json, "fingerprints", fingerprint, "fingerprint")
        && opdump_license_check_activation(json)
        && opdump_license_check_revocation(json)
        && opdump_license_check_policy(json);
}

static int opdump_b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static unsigned char *opdump_base64_decode(const char *in, size_t *out_len)
{
    size_t in_len = strlen(in);
    unsigned char *out = (unsigned char *) malloc(in_len / 4 * 3 + 3);
    if (!out) {
        return NULL;
    }
    size_t o = 0;
    int vals[4];
    int n = 0;
    for (size_t i = 0; i < in_len; i++) {
        char c = in[i];
        if (c == '=' || c == '\n' || c == '\r') {
            continue;
        }
        int v = opdump_b64_val(c);
        if (v < 0) {
            free(out);
            return NULL;
        }
        vals[n++] = v;
        if (n == 4) {
            out[o++] = (unsigned char)((vals[0] << 2) | (vals[1] >> 4));
            out[o++] = (unsigned char)((vals[1] << 4) | (vals[2] >> 2));
            out[o++] = (unsigned char)((vals[2] << 6) | vals[3]);
            n = 0;
        }
    }
    if (n == 1) {
        free(out);
        return NULL;
    } else if (n == 2) {
        out[o++] = (unsigned char)((vals[0] << 2) | (vals[1] >> 4));
    } else if (n == 3) {
        out[o++] = (unsigned char)((vals[0] << 2) | (vals[1] >> 4));
        out[o++] = (unsigned char)((vals[1] << 4) | (vals[2] >> 2));
    }
    *out_len = o;
    return out;
}

/* RSA-OAEP-SHA256 unwrap via the EVP_PKEY API. The digest is set explicitly
 * on both OAEP and MGF1 rather than relying on OpenSSL's legacy SHA-1
 * default for RSA_PKCS1_OAEP_PADDING -- bytecode-dump wraps the DEK the same
 * way via `openssl pkeyutl -pkeyopt rsa_oaep_md:sha256 -pkeyopt rsa_mgf1_md:sha256`
 * (PHP's openssl_public_encrypt() has no way to choose the OAEP digest, so
 * wrapping shells out to the openssl CLI instead of using that function). */
static bool opdump_rsa_unwrap_dek(
    const char *private_key_path, const char *passphrase,
    const unsigned char *wrapped, size_t wrapped_len,
    unsigned char dek[32]
) {
    FILE *f = fopen(private_key_path, "rb");
    if (!f) {
        php_error_docref(NULL, E_WARNING, "opdump: cannot open OPDUMP_LICENSE_KEY_FILE %s", private_key_path);
        return false;
    }
    EVP_PKEY *pkey = PEM_read_PrivateKey(f, NULL, NULL, (void *) passphrase);
    fclose(f);
    if (!pkey) {
        php_error_docref(NULL, E_WARNING, "opdump: cannot parse RSA private key %s (wrong passphrase?)", private_key_path);
        return false;
    }

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, NULL);
    bool ok = ctx
        && EVP_PKEY_decrypt_init(ctx) == 1
        && EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) == 1
        && EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) == 1
        && EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256()) == 1;

    /* The OpenSSL 3.0 provider path requires querying the maximum output
     * size first (out=NULL) before the real decrypt call -- it rejects a
     * buffer pre-sized to the known 32-byte plaintext length with a
     * generic "bad length" error, since it can't tell that's sufficient
     * before unpadding. The query conservatively returns the RSA modulus
     * size (e.g. 512 bytes for RSA-4096). */
    size_t max_len = 0;
    ok = ok && EVP_PKEY_decrypt(ctx, NULL, &max_len, wrapped, wrapped_len) == 1 && max_len > 0;

    unsigned char *out = ok ? (unsigned char *) malloc(max_len) : NULL;
    ok = ok && out != NULL;

    size_t out_len = max_len;
    ok = ok && EVP_PKEY_decrypt(ctx, out, &out_len, wrapped, wrapped_len) == 1 && out_len == 32;
    if (ok) {
        memcpy(dek, out, 32);
    }
    if (out) {
        OPENSSL_cleanse(out, max_len);
        free(out);
    }
    if (ctx) {
        EVP_PKEY_CTX_free(ctx);
    }
    EVP_PKEY_free(pkey);
    return ok;
}

/* Cached for the process lifetime: load-tree mode may decrypt many
 * containers per request, and RSA-OAEP decrypt is comparatively expensive.
 * The cache itself is key material and gets cleansed at MSHUTDOWN
 * (Rung D) rather than immediately, since it must survive for the whole
 * request. */
static bool opdump_license_ikm_ready = false;
static bool opdump_license_ikm_failed = false;
static unsigned char opdump_license_ikm_cache[32];

static bool opdump_license_resolve_ikm(const char *key_file, unsigned char ikm[32])
{
    if (opdump_license_ikm_ready) {
        memcpy(ikm, opdump_license_ikm_cache, 32);
        return true;
    }
    if (opdump_license_ikm_failed) {
        return false;
    }
    opdump_license_ikm_failed = true; /* pessimistic default; cleared below on success */

    const char *license_file = getenv("OPDUMP_LICENSE_FILE");
    char *derived_path = NULL;
    if (!license_file || !license_file[0]) {
        const char *map = getenv("OPDUMP_MAP");
        if (!map || !map[0]) {
            map = getenv("BYTECODE_MAP");
        }
        if (map && map[0]) {
            char *map_dir = opdump_dirname_dup(map);
            derived_path = opdump_join_path(map_dir, "bytecode.license.json");
            free(map_dir);
            license_file = derived_path;
        } else if (opdump_current_container_path && opdump_current_container_path[0]) {
            char *container_dir = opdump_dirname_dup(opdump_current_container_path);
            derived_path = opdump_join_path(container_dir, "bytecode.license.json");
            free(container_dir);
            license_file = derived_path;
        }
    }
    if (!license_file || !license_file[0]) {
        php_error_docref(NULL, E_WARNING, "opdump: OPDUMP_LICENSE_KEY_FILE set but no OPDUMP_LICENSE_FILE, OPDUMP_MAP, or adjacent bytecode.license.json to derive one from");
        free(derived_path);
        return false;
    }

    char *json = opdump_read_text_file(license_file);
    if (!json) {
        php_error_docref(NULL, E_WARNING, "opdump: cannot read license file %s", license_file);
        free(derived_path);
        return false;
    }

    /* Before trusting ANY license constraint (expiry, domains, machine_id, ...),
     * require the vendor seal to verify when a vendor public key is configured.
     * The constraint fields live in plaintext in bytecode.license.json, and in
     * license mode the operator holds the RSA private key that unwraps the DEK
     * -- so without a vendor signature they could simply edit expires_at or
     * delete the domain/machine rules. The seal is what makes these binding. */
    {
        char *license_dir = opdump_dirname_dup(license_file);
        bool sealed = opdump_verify_vendor_seal(license_dir);
        free(license_dir);
        if (!sealed) {
            free(json);
            free(derived_path);
            return false;
        }
    }

    if (!opdump_license_check_constraints(json)) {
        free(json);
        free(derived_path);
        return false;
    }

    if (!opdump_policy_check_machine_id(json, "license")) {
        free(json);
        free(derived_path);
        return false;
    }

    char *wrapped_b64 = opdump_json_extract_string(json, "wrapped_dek");
    free(json);
    if (!wrapped_b64) {
        php_error_docref(NULL, E_WARNING, "opdump: license file %s missing wrapped_dek", license_file);
        free(derived_path);
        return false;
    }

    size_t wrapped_len = 0;
    unsigned char *wrapped = opdump_base64_decode(wrapped_b64, &wrapped_len);
    free(wrapped_b64);
    free(derived_path);
    if (!wrapped) {
        php_error_docref(NULL, E_WARNING, "opdump: license file %s has malformed wrapped_dek", license_file);
        return false;
    }

    const char *passphrase = getenv("OPDUMP_LICENSE_KEY_PASSPHRASE");
    bool unwrapped = opdump_rsa_unwrap_dek(key_file, passphrase, wrapped, wrapped_len, opdump_license_ikm_cache);
    OPENSSL_cleanse(wrapped, wrapped_len);
    free(wrapped);
    if (!unwrapped) {
        php_error_docref(NULL, E_WARNING, "opdump: failed to unwrap license DEK using %s", key_file);
        return false;
    }

    opdump_license_ikm_failed = false;
    opdump_license_ikm_ready = true;
    opdump_lock_mem(opdump_license_ikm_cache, sizeof(opdump_license_ikm_cache));
    memcpy(ikm, opdump_license_ikm_cache, 32);
    return true;
}

/* ---- vendor seal: Ed25519 signature over the whole package (Rung D) ----
 *
 * bytecode.manifest.sig (below) is an HMAC keyed by the same IKM that the
 * running server already possesses -- it stops a THIRD party without the key
 * from swapping map entries, but it does NOT stop the license holder, who in
 * license mode holds the RSA private key that unwraps the DEK, from editing
 * the plaintext constraints in bytecode.license.json (expiry, domains,
 * machine_id, ...) and re-HMAC'ing the manifest. The vendor seal closes that
 * gap: bytecode-dump signs sha256(manifest) + sha256(map) + sha256(license)
 * with the vendor's Ed25519 PRIVATE key, and the loader verifies it against a
 * PUBLIC key the vendor controls -- compiled into the loader
 * (OPDUMP_VENDOR_PUBKEY_HEX, preferred) or, failing that, a PEM at
 * OPDUMP_VENDOR_PUBKEY_FILE. When such an anchor exists, a missing or invalid
 * seal fails closed. */

static bool opdump_file_exists(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    fclose(f);
    return true;
}

/* SHA-256 of a file's bytes, written as 64 lowercase hex chars + NUL into out.
 * Returns false if the file cannot be read or hashed. */
static bool opdump_sha256_hex_file(const char *path, char out[65])
{
    if (!path || !path[0]) {
        return false;
    }
    size_t len = 0;
    unsigned char *raw = opdump_read_file_bytes(path, &len);
    if (!raw) {
        return false;
    }
    unsigned char digest[32];
    unsigned int digest_len = 0;
    bool ok = EVP_Digest(raw, len, digest, &digest_len, EVP_sha256(), NULL) == 1 && digest_len == 32;
    OPENSSL_cleanse(raw, len);
    free(raw);
    if (!ok) {
        return false;
    }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[64] = '\0';
    return true;
}

static bool opdump_sha256_hex_bytes(const unsigned char *bytes, size_t len, char out[65])
{
    unsigned char digest[32];
    unsigned int digest_len = 0;
    bool ok = EVP_Digest(bytes, len, digest, &digest_len, EVP_sha256(), NULL) == 1 && digest_len == 32;
    if (!ok) {
        return false;
    }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[64] = '\0';
    return true;
}

static char *opdump_seal_policy_message(const char *json)
{
    const char *keys[] = {
        "machine_id",
        "domains",
        "ips",
        "hostnames",
        "fingerprints",
        "expires_at",
        "license_id",
        "activation_token",
        "revocation_url",
        "unlock_policy",
        NULL
    };
    size_t len = strlen("bytecode-seal-policy-v1\n");
    char *values[10] = {0};
    for (int i = 0; keys[i]; i++) {
        values[i] = opdump_json_extract_string(json, keys[i]);
        len += strlen(keys[i]) + 2 + (values[i] && values[i][0] ? strlen(values[i]) : 1);
    }
    char *msg = (char *) malloc(len + 1);
    if (!msg) {
        for (int i = 0; keys[i]; i++) free(values[i]);
        return NULL;
    }
    strcpy(msg, "bytecode-seal-policy-v1\n");
    for (int i = 0; keys[i]; i++) {
        strcat(msg, keys[i]);
        strcat(msg, ":");
        strcat(msg, values[i] && values[i][0] ? values[i] : "-");
        strcat(msg, "\n");
        free(values[i]);
    }
    return msg;
}

static bool opdump_policy_check_machine_id(const char *json, const char *label)
{
    char *machine_id = opdump_json_extract_string(json, "machine_id");
    if (!machine_id || !machine_id[0]) {
        free(machine_id);
        return true;
    }
    const char *actual_machine_id = getenv("OPDUMP_MACHINE_ID");
    bool ok = actual_machine_id && strcmp(machine_id, actual_machine_id) == 0;
    if (!ok) {
        php_error_docref(NULL, E_WARNING, "opdump: %s machine_id does not match this server", label);
    }
    free(machine_id);
    return ok;
}

/* Resolves the vendor public-key trust anchor into *out.
 * Returns: 1 anchor resolved (caller must EVP_PKEY_free *out);
 *          0 no anchor configured (seal not enforced);
 *         -1 anchor configured but unusable (fail closed). */
static int opdump_vendor_pubkey(EVP_PKEY **out)
{
    *out = NULL;
    const char *hex = OPDUMP_VENDOR_PUBKEY_HEX;
    if (hex && hex[0]) {
        unsigned char raw[32];
        if (strlen(hex) != 64 || !opdump_hex_decode(hex, raw, 32)) {
            php_error_docref(NULL, E_WARNING, "opdump: compiled-in vendor public key is not 64 hex chars");
            return -1;
        }
        EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, raw, 32);
        OPENSSL_cleanse(raw, sizeof(raw));
        if (!pkey) {
            php_error_docref(NULL, E_WARNING, "opdump: cannot build Ed25519 key from compiled-in vendor public key");
            return -1;
        }
        *out = pkey;
        return 1;
    }
    const char *file = getenv("OPDUMP_VENDOR_PUBKEY_FILE");
    if (file && file[0]) {
        FILE *f = fopen(file, "rb");
        if (!f) {
            php_error_docref(NULL, E_WARNING, "opdump: cannot open OPDUMP_VENDOR_PUBKEY_FILE %s", file);
            return -1;
        }
        EVP_PKEY *pkey = PEM_read_PUBKEY(f, NULL, NULL, NULL);
        fclose(f);
        if (!pkey) {
            php_error_docref(NULL, E_WARNING, "opdump: cannot parse Ed25519 public key PEM %s", file);
            return -1;
        }
        *out = pkey;
        return 1;
    }
    return 0;
}

static bool opdump_verify_vendor_seal(const char *dir)
{
    EVP_PKEY *pkey = NULL;
    int anchor = opdump_vendor_pubkey(&pkey);
    if (anchor == 0) {
        return true; /* no trust anchor configured -- seal not enforced (legacy) */
    }
    if (anchor < 0) {
        return false; /* anchor configured but unusable -- fail closed */
    }

    char *seal_path = opdump_join_path(dir, "bytecode.seal.json");
    char *manifest_path = opdump_join_path(dir, "bytecode.manifest.json");
    char *map_path = opdump_join_path(dir, "bytecode.map");
    char *license_path = opdump_join_path(dir, "bytecode.license.json");
    unsigned char *sig = NULL;
    char *fmt = NULL, *alg = NULL, *sig_b64 = NULL;
    char *seal_manifest = NULL, *seal_map = NULL, *seal_license = NULL, *seal_policy = NULL;
    char *seal = NULL;
    char *policy_msg = NULL;
    bool ok = false;

    /* Existence-check first so a legitimately missing seal fails closed with a
     * clear diagnostic, rather than the generic read-error opdump_read_text_file
     * would raise. */
    if (!opdump_file_exists(seal_path)) {
        php_error_docref(NULL, E_WARNING,
            "opdump: vendor public key is configured but no bytecode.seal.json in %s -- refusing to load", dir);
        goto cleanup;
    }
    if (!opdump_file_exists(manifest_path) || !opdump_file_exists(map_path)) {
        php_error_docref(NULL, E_WARNING,
            "opdump: sealed package is missing bytecode.manifest.json or bytecode.map in %s", dir);
        goto cleanup;
    }
    seal = opdump_read_text_file(seal_path);
    if (!seal) {
        php_error_docref(NULL, E_WARNING, "opdump: cannot read bytecode.seal.json in %s", dir);
        goto cleanup;
    }

    fmt = opdump_json_extract_string(seal, "format");
    alg = opdump_json_extract_string(seal, "alg");
    if (!fmt || strcmp(fmt, "bytecode-seal-v1") != 0 || !alg || strcmp(alg, "Ed25519") != 0) {
        php_error_docref(NULL, E_WARNING, "opdump: unsupported or malformed bytecode.seal.json in %s", dir);
        goto cleanup;
    }

    seal_manifest = opdump_json_extract_string(seal, "manifest_sha256");
    seal_map = opdump_json_extract_string(seal, "map_sha256");
    seal_license = opdump_json_extract_string(seal, "license_sha256"); /* optional */
    seal_policy = opdump_json_extract_string(seal, "policy_sha256"); /* optional */
    sig_b64 = opdump_json_extract_string(seal, "signature");
    if (!seal_manifest || !seal_map || !sig_b64) {
        php_error_docref(NULL, E_WARNING, "opdump: bytecode.seal.json in %s is missing required fields", dir);
        goto cleanup;
    }

    /* Bind the seal to the actual files on disk: recompute each digest and
     * require it to equal what the seal claims. A tampered manifest/map/license
     * changes its digest and is rejected here before the signature is checked. */
    char actual_manifest[65], actual_map[65];
    if (!opdump_sha256_hex_file(manifest_path, actual_manifest) ||
        strcasecmp(actual_manifest, seal_manifest) != 0) {
        php_error_docref(NULL, E_WARNING, "opdump: bytecode.manifest.json digest does not match seal in %s", dir);
        goto cleanup;
    }
    if (!opdump_sha256_hex_file(map_path, actual_map) ||
        strcasecmp(actual_map, seal_map) != 0) {
        php_error_docref(NULL, E_WARNING, "opdump: bytecode.map digest does not match seal in %s", dir);
        goto cleanup;
    }

    bool license_present = opdump_file_exists(license_path);
    if (license_present) {
        char actual_license[65];
        if (!seal_license ||
            !opdump_sha256_hex_file(license_path, actual_license) ||
            strcasecmp(actual_license, seal_license) != 0) {
            php_error_docref(NULL, E_WARNING, "opdump: bytecode.license.json digest does not match seal in %s", dir);
            goto cleanup;
        }
    } else if (seal_license && seal_license[0]) {
        /* Seal was issued over a license that has since been removed. */
        php_error_docref(NULL, E_WARNING, "opdump: seal covers a bytecode.license.json that is missing in %s", dir);
        goto cleanup;
    }

    if (seal_policy && seal_policy[0]) {
        char actual_policy[65];
        policy_msg = opdump_seal_policy_message(seal);
        if (!policy_msg ||
            !opdump_sha256_hex_bytes((const unsigned char *) policy_msg, strlen(policy_msg), actual_policy) ||
            strcasecmp(actual_policy, seal_policy) != 0) {
            php_error_docref(NULL, E_WARNING, "opdump: bytecode.seal.json policy digest does not match its signed policy fields in %s", dir);
            goto cleanup;
        }
        if (!opdump_license_check_constraints(seal) || !opdump_policy_check_machine_id(seal, "seal policy")) {
            goto cleanup;
        }
    }

    /* Reconstruct the exact canonical message bytecode-dump signed. */
    size_t msg_cap = 512;
    char *msg = (char *) malloc(msg_cap);
    if (!msg) {
        goto cleanup;
    }
    int msg_len = snprintf(msg, msg_cap,
        "bytecode-seal-v1\nmanifest:%s\nmap:%s\nlicense:%s\n",
        seal_manifest, seal_map, (license_present && seal_license) ? seal_license : "-");
    if (msg_len > 0 && seal_policy && seal_policy[0]) {
        int extra = snprintf(msg + msg_len, msg_cap - (size_t) msg_len, "policy:%s\n", seal_policy);
        if (extra > 0) {
            msg_len += extra;
        } else {
            msg_len = -1;
        }
    }
    if (msg_len <= 0 || (size_t)msg_len >= msg_cap) {
        php_error_docref(NULL, E_WARNING, "opdump: seal message construction failed in %s", dir);
        free(msg);
        goto cleanup;
    }

    size_t sig_len = 0;
    sig = opdump_base64_decode(sig_b64, &sig_len);
    if (!sig || sig_len != 64) {
        php_error_docref(NULL, E_WARNING, "opdump: malformed Ed25519 signature in bytecode.seal.json in %s", dir);
        goto cleanup;
    }

    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    ok = mdctx
        && EVP_DigestVerifyInit(mdctx, NULL, NULL, NULL, pkey) == 1
        && EVP_DigestVerify(mdctx, sig, sig_len, (const unsigned char *) msg, (size_t) msg_len) == 1;
    free(msg);
    if (mdctx) {
        EVP_MD_CTX_free(mdctx);
    }
    if (!ok) {
        php_error_docref(NULL, E_WARNING,
            "opdump: bytecode.seal.json signature is not valid for the vendor public key in %s -- refusing to load", dir);
    }

cleanup:
    if (pkey) {
        EVP_PKEY_free(pkey);
    }
    free(sig);
    free(sig_b64);
    free(seal_manifest);
    free(seal_map);
    free(seal_license);
    free(seal_policy);
    free(policy_msg);
    free(fmt);
    free(alg);
    free(seal);
    free(seal_path);
    free(manifest_path);
    free(map_path);
    free(license_path);
    return ok;
}

/* ---- manifest/map authentication (Rung C) ----
 *
 * BYTC's per-file GCM tag only authenticates that one container's own bytes.
 * Nothing previously stopped bytecode.map itself from being edited to point
 * a source path at a *different*, still-validly-encrypted container -- each
 * file would still decrypt and authenticate cleanly on its own. This
 * verifies bytecode.manifest.json + bytecode.map together against
 * bytecode.manifest.sig (an HMAC-SHA256 under an HKDF-derived signing key,
 * written by bytecode-dump) before a single map entry is trusted. */
static bool opdump_verify_manifest_signature(const char *map_dir, const char *map_path)
{
    char *manifest_path = opdump_join_path(map_dir, "bytecode.manifest.json");
    char *sig_path = opdump_join_path(map_dir, "bytecode.manifest.sig");
    char *manifest_text = manifest_path ? opdump_read_text_file(manifest_path) : NULL;
    char *map_text = opdump_read_text_file(map_path);
    char *sig_text = sig_path ? opdump_read_text_file(sig_path) : NULL;
    bool ok = false;

    if (!manifest_text || !map_text || !sig_text) {
        php_error_docref(NULL, E_WARNING,
            "opdump: cannot verify bytecode.manifest.sig in %s (missing bytecode.manifest.json, bytecode.map, or bytecode.manifest.sig)",
            map_dir);
        goto cleanup;
    }

    size_t sig_len = strlen(sig_text);
    while (sig_len > 0 && (sig_text[sig_len - 1] == '\n' || sig_text[sig_len - 1] == '\r' || sig_text[sig_len - 1] == ' ')) {
        sig_text[--sig_len] = '\0';
    }

    unsigned char expected_sig[32];
    if (!opdump_hex_decode(sig_text, expected_sig, sizeof(expected_sig))) {
        php_error_docref(NULL, E_WARNING, "opdump: malformed bytecode.manifest.sig in %s", map_dir);
        goto cleanup;
    }

    unsigned char ikm[32];
    if (!opdump_resolve_ikm(ikm)) {
        php_error_docref(NULL, E_WARNING, "opdump: no key material available to verify bytecode.manifest.sig in %s", map_dir);
        goto cleanup;
    }
    unsigned char sig_key[32];
    const unsigned char empty_salt[1] = {0};
    bool derived = opdump_hkdf_sha256(ikm, sizeof(ikm), empty_salt, 0, "bytecode-manifest-auth", sig_key);
    OPENSSL_cleanse(ikm, sizeof(ikm));
    if (!derived) {
        php_error_docref(NULL, E_WARNING, "opdump: manifest signing key derivation failed");
        goto cleanup;
    }

    size_t manifest_len = strlen(manifest_text);
    size_t map_len = strlen(map_text);
    size_t msg_len = manifest_len + 1 + map_len;
    unsigned char *msg = (unsigned char *) malloc(msg_len ? msg_len : 1);
    if (msg) {
        memcpy(msg, manifest_text, manifest_len);
        msg[manifest_len] = 0x00;
        memcpy(msg + manifest_len + 1, map_text, map_len);

        unsigned char actual_sig[32];
        unsigned int actual_sig_len = 0;
        if (HMAC(EVP_sha256(), sig_key, sizeof(sig_key), msg, msg_len, actual_sig, &actual_sig_len)
            && actual_sig_len == sizeof(actual_sig)) {
            ok = CRYPTO_memcmp(actual_sig, expected_sig, sizeof(actual_sig)) == 0;
        }
        OPENSSL_cleanse(msg, msg_len);
        free(msg);
    }
    OPENSSL_cleanse(sig_key, sizeof(sig_key));

    if (!ok) {
        php_error_docref(NULL, E_WARNING, "opdump: bytecode.manifest.sig mismatch in %s -- refusing to trust bytecode.map", map_dir);
    }

cleanup:
    free(manifest_path);
    free(sig_path);
    free(manifest_text);
    free(map_text);
    free(sig_text);
    return ok;
}

static bool opdump_decrypt_bytc(const unsigned char *buf, size_t len, unsigned char **out, size_t *out_len)
{
    size_t off = 4;
    uint32_t container_version, php_version_id, backend_id_len, backend_format_len;
    const unsigned char *backend_id, *backend_format, *nonce, *tag, *ciphertext;
    uint32_t nonce_len, tag_len;
    uint64_t ciphertext_len_u64;

    if (!opdump_take_u32(buf, len, &off, &container_version)
        || !opdump_take_u32(buf, len, &off, &php_version_id)
        || !opdump_take_u32(buf, len, &off, &backend_id_len)
        || !opdump_take_bytes(buf, len, &off, backend_id_len, &backend_id)
        || !opdump_take_u32(buf, len, &off, &backend_format_len)
        || !opdump_take_bytes(buf, len, &off, backend_format_len, &backend_format)) {
        php_error_docref(NULL, E_ERROR, "opdump: malformed BYTC header");
        return false;
    }

    if (container_version != 1 && container_version != 2) {
        php_error_docref(NULL, E_ERROR, "opdump: unsupported BYTC version %u", container_version);
        return false;
    }
    if (php_version_id != PHP_VERSION_ID) {
        php_error_docref(NULL, E_ERROR, "opdump: BYTC PHP_VERSION_ID %u does not match running %u", php_version_id, PHP_VERSION_ID);
        return false;
    }
    if (backend_id_len != sizeof("php-zend-opdump") - 1
        || memcmp(backend_id, "php-zend-opdump", backend_id_len) != 0
        || backend_format_len != 4
        || memcmp(backend_format, OPDUMP_MAGIC, 4) != 0) {
        php_error_docref(NULL, E_ERROR, "opdump: unsupported BYTC backend");
        return false;
    }

    /* v2 adds an authenticated cipher_id + key_id before the nonce/tag/ciphertext
     * trailer; v1's layout is unchanged from Phase 1. */
    uint32_t cipher_id = 1;
    const unsigned char *key_id = NULL;
    uint32_t key_id_len = 0;
    if (container_version == 2) {
        if (!opdump_take_u32(buf, len, &off, &cipher_id)
            || !opdump_take_u32(buf, len, &off, &key_id_len)
            || !opdump_take_bytes(buf, len, &off, key_id_len, &key_id)) {
            php_error_docref(NULL, E_ERROR, "opdump: malformed BYTC2 key header");
            return false;
        }
        if (cipher_id != 1) {
            php_error_docref(NULL, E_ERROR, "opdump: unsupported BYTC2 cipher_id %u", cipher_id);
            return false;
        }
    }
    size_t aad_len = off;

    if (!opdump_take_u32(buf, len, &off, &nonce_len)
        || !opdump_take_bytes(buf, len, &off, nonce_len, &nonce)
        || !opdump_take_u32(buf, len, &off, &tag_len)
        || !opdump_take_bytes(buf, len, &off, tag_len, &tag)
        || !opdump_take_u64(buf, len, &off, &ciphertext_len_u64)) {
        php_error_docref(NULL, E_ERROR, "opdump: malformed BYTC payload metadata");
        return false;
    }
    if (ciphertext_len_u64 > SIZE_MAX) {
        php_error_docref(NULL, E_ERROR, "opdump: BYTC ciphertext too large");
        return false;
    }
    size_t ciphertext_len = (size_t)ciphertext_len_u64;
    if (!opdump_take_bytes(buf, len, &off, ciphertext_len, &ciphertext) || off != len) {
        php_error_docref(NULL, E_ERROR, "opdump: malformed BYTC ciphertext");
        return false;
    }
    if (nonce_len != 12 || tag_len != 16) {
        php_error_docref(NULL, E_ERROR, "opdump: unsupported BYTC nonce/tag length");
        return false;
    }

    unsigned char key[32];
    if (container_version == 1) {
        if (!opdump_key_from_env(key)) {
            php_error_docref(NULL, E_ERROR, "opdump: BYTECODE_KEY/OPDUMP_KEY must be 64 hex chars for BYTC load");
            return false;
        }
    } else {
        unsigned char ikm[32];
        if (!opdump_resolve_ikm(ikm)) {
            php_error_docref(NULL, E_ERROR, "opdump: no key material available for BYTC2 load (checked license and BYTECODE_KEY/OPDUMP_KEY)");
            return false;
        }
        bool derived = opdump_hkdf_sha256(ikm, sizeof(ikm), key_id, key_id_len, "bytecode-v2", key);
        OPENSSL_cleanse(ikm, sizeof(ikm));
        if (!derived) {
            php_error_docref(NULL, E_ERROR, "opdump: key derivation failed for BYTC2 container");
            return false;
        }
    }

    unsigned char *plain = (unsigned char *) malloc(ciphertext_len ? ciphertext_len : 1);
    if (!plain) {
        OPENSSL_cleanse(key, sizeof(key));
        return false;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len1 = 0, len2 = 0;
    bool ok = ctx
        && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)nonce_len, NULL) == 1
        && EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) == 1
        && EVP_DecryptUpdate(ctx, NULL, &len1, buf, (int)aad_len) == 1
        && EVP_DecryptUpdate(ctx, plain, &len1, ciphertext, (int)ciphertext_len) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, (int)tag_len, (void *)tag) == 1
        && EVP_DecryptFinal_ex(ctx, plain + len1, &len2) == 1;

    OPENSSL_cleanse(key, sizeof(key));
    if (ctx) {
        EVP_CIPHER_CTX_free(ctx);
    }
    if (!ok) {
        free(plain);
        php_error_docref(NULL, E_ERROR, "opdump: BYTC authentication/decryption failed");
        return false;
    }

    *out = plain;
    *out_len = (size_t)(len1 + len2);
    return true;
}

/* ---- dump ---- */

/* Maximum constant-expression AST depth we will (de)serialize, to bound both
 * output size and load-time recursion on a hostile container. */
#define OPDUMP_MAX_AST_DEPTH 256

static void opdump_write_array(FILE *f, HashTable *ht);
static void opdump_read_array(FILE *f, zval *zv);
static void opdump_write_ast(FILE *f, zend_ast *ast, int depth);
static zend_ast *opdump_read_ast(FILE *f, int depth);
static void opdump_free_ast(zend_ast *ast);

static void opdump_write_literal(FILE *f, zval *zv)
{
    switch (Z_TYPE_P(zv)) {
        case IS_UNDEF: w_u8(f, OPDUMP_LIT_UNDEF); break;
        case IS_NULL:  w_u8(f, OPDUMP_LIT_NULL); break;
        case IS_TRUE:  w_u8(f, OPDUMP_LIT_TRUE); break;
        case IS_FALSE: w_u8(f, OPDUMP_LIT_FALSE); break;
        case IS_LONG:  w_u8(f, OPDUMP_LIT_LONG); w_u32(f, (uint32_t)Z_LVAL_P(zv)); w_u32(f, (uint32_t)(((zend_long)Z_LVAL_P(zv)) >> 32)); break;
        case IS_DOUBLE: w_u8(f, OPDUMP_LIT_DOUBLE); w_dbl(f, Z_DVAL_P(zv)); break;
        case IS_STRING: w_u8(f, OPDUMP_LIT_STRING); w_str(f, Z_STRVAL_P(zv), Z_STRLEN_P(zv)); break;
        case IS_ARRAY:  w_u8(f, OPDUMP_LIT_ARRAY); opdump_write_array(f, Z_ARRVAL_P(zv)); break;
        case IS_CONSTANT_AST: w_u8(f, OPDUMP_LIT_AST); opdump_write_ast(f, Z_ASTVAL_P(zv), 0); break;
        default:
            /* Fail closed: a value shape we cannot faithfully round-trip must
             * abort the dump, never be silently replaced with NULL -- a silent
             * substitution would ship subtly wrong behaviour to production. Set
             * OPDUMP_ALLOW_LOSSY=1 to opt into the old placeholder behaviour for
             * throwaway experiments only. */
            if (getenv("OPDUMP_ALLOW_LOSSY")) {
                php_error_docref(NULL, E_WARNING,
                    "opdump: unsupported literal type %d -- writing NULL placeholder (OPDUMP_ALLOW_LOSSY)",
                    Z_TYPE_P(zv));
                w_u8(f, OPDUMP_LIT_NULL);
            } else {
                php_error_docref(NULL, E_ERROR,
                    "opdump: unsupported value type %d in a compiled literal/constant; refusing to emit a lossy container (set OPDUMP_ALLOW_LOSSY=1 to override)",
                    Z_TYPE_P(zv));
            }
    }
}

static void opdump_read_literal(FILE *f, zval *zv)
{
    uint8_t tag = r_u8(f);
    switch (tag) {
        case OPDUMP_LIT_UNDEF: ZVAL_UNDEF(zv); break;
        case OPDUMP_LIT_NULL:  ZVAL_NULL(zv); break;
        case OPDUMP_LIT_TRUE:  ZVAL_TRUE(zv); break;
        case OPDUMP_LIT_FALSE: ZVAL_FALSE(zv); break;
        case OPDUMP_LIT_LONG: {
            uint32_t lo = r_u32(f);
            uint32_t hi = r_u32(f);
            zend_long v = (zend_long)(((uint64_t)hi << 32) | lo);
            ZVAL_LONG(zv, v);
            break;
        }
        case OPDUMP_LIT_DOUBLE: ZVAL_DOUBLE(zv, r_dbl(f)); break;
        case OPDUMP_LIT_STRING: {
            uint32_t slen;
            char *s = r_str(f, &slen);
            zend_string *zs = zend_string_init(s, slen, 0);
            free(s);
            zs = zend_new_interned_string(zs);
            zend_string_hash_val(zs);
            ZVAL_STR(zv, zs);
            break;
        }
        case OPDUMP_LIT_ARRAY: opdump_read_array(f, zv); break;
        case OPDUMP_LIT_AST: {
            zend_ast *tmp = opdump_read_ast(f, 0);
            if (tmp) {
                /* zend_ast_copy consolidates the temporary tree into a single
                 * refcounted zend_ast_ref -- exactly the shape a persistent
                 * constant/default-value AST needs -- deep-copying (addref'ing)
                 * every embedded zval, so we then release our temporary tree. */
                zend_ast_ref *ref = zend_ast_copy(tmp);
                opdump_free_ast(tmp);
                ZVAL_AST(zv, ref);
            } else {
                ZVAL_NULL(zv);
            }
            break;
        }
        default:
            ZVAL_NULL(zv);
    }
}

/* ---- array + constant-AST value (de)serialization ----
 *
 * op_array literals, class-constant values, and property/static-member
 * defaults are ordinary zvals, but for anything richer than a scalar they are
 * either an IS_ARRAY (a constant array operand) or, far more often, an
 * IS_CONSTANT_AST: a deferred expression (`const A = [1, 2]`, `= self::X | 4`,
 * `= PHP_INT_MAX`, `new Foo()`) that PHP evaluates lazily the first time the
 * owning constant/property is read. Both can nest arbitrarily, so both codecs
 * recurse back through opdump_{write,read}_literal. */
static void opdump_write_array(FILE *f, HashTable *ht)
{
    uint32_t n = ht ? zend_hash_num_elements(ht) : 0;
    w_u32(f, n);
    if (!n) {
        return;
    }
    zend_ulong num_key;
    zend_string *str_key;
    zval *val;
    ZEND_HASH_FOREACH_KEY_VAL(ht, num_key, str_key, val) {
        if (str_key) {
            w_u8(f, 1);
            w_zstr(f, str_key);
        } else {
            w_u8(f, 0);
            w_u32(f, (uint32_t) num_key);
            w_u32(f, (uint32_t) (((uint64_t) num_key) >> 32));
        }
        ZVAL_DEREF(val);
        opdump_write_literal(f, val);
    } ZEND_HASH_FOREACH_END();
}

static void opdump_read_array(FILE *f, zval *zv)
{
    uint32_t n = r_u32(f);
    opdump_guard_u32(n, OPDUMP_MAX_LITERALS, "array element");
    zend_array *arr = zend_new_array(n);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t is_str = r_u8(f);
        zval tmp;
        if (is_str) {
            zend_string *k = r_zstr(f, false);
            opdump_read_literal(f, &tmp);
            zend_hash_update(arr, k, &tmp);
            zend_string_release(k);
        } else {
            uint32_t lo = r_u32(f);
            uint32_t hi = r_u32(f);
            zend_ulong k = ((zend_ulong) hi << 32) | lo;
            opdump_read_literal(f, &tmp);
            zend_hash_index_update(arr, k, &tmp);
        }
    }
    ZVAL_ARR(zv, arr);
}

/* zend_ast kinds encode their own shape: ZEND_AST_ZVAL/ZEND_AST_CONSTANT carry
 * a zval, list kinds carry a runtime child count, and every other kind's fixed
 * child count is (kind >> ZEND_AST_NUM_CHILDREN_SHIFT). That lets one generic
 * walker cover every node that can appear in a constant expression without
 * enumerating opcodes. */
static void opdump_write_ast(FILE *f, zend_ast *ast, int depth)
{
    if (!ast || depth > OPDUMP_MAX_AST_DEPTH) {
        w_u8(f, 0);
        return;
    }
    w_u8(f, 1);
    w_u32(f, (uint32_t) ast->kind);
    w_u32(f, (uint32_t) ast->attr);

    if (ast->kind == ZEND_AST_ZVAL || ast->kind == ZEND_AST_CONSTANT) {
        zval *zv = zend_ast_get_zval(ast);
        w_u32(f, Z_LINENO_P(zv));
        opdump_write_literal(f, zv);
    } else if (zend_ast_is_list(ast)) {
        zend_ast_list *list = zend_ast_get_list(ast);
        w_u32(f, list->lineno);
        w_u32(f, list->children);
        for (uint32_t i = 0; i < list->children; i++) {
            opdump_write_ast(f, list->child[i], depth + 1);
        }
    } else {
        w_u32(f, ast->lineno);
        uint32_t children = zend_ast_get_num_children(ast);
        w_u32(f, children);
        for (uint32_t i = 0; i < children; i++) {
            opdump_write_ast(f, ast->child[i], depth + 1);
        }
    }
}

static zend_ast *opdump_read_ast(FILE *f, int depth)
{
    if (!r_u8(f)) {
        return NULL;
    }
    if (depth > OPDUMP_MAX_AST_DEPTH) {
        php_error_docref(NULL, E_ERROR, "opdump: constant-expression AST nesting too deep");
        return NULL;
    }
    uint32_t kind = r_u32(f);
    uint32_t attr = r_u32(f);

    if (kind == ZEND_AST_ZVAL || kind == ZEND_AST_CONSTANT) {
        uint32_t lineno = r_u32(f);
        zend_ast_zval *node = (zend_ast_zval *) emalloc(sizeof(zend_ast_zval));
        node->kind = (zend_ast_kind) kind;
        node->attr = (zend_ast_attr) attr;
        opdump_read_literal(f, &node->val);
        Z_LINENO(node->val) = lineno;
        return (zend_ast *) node;
    }

    if (kind & (1 << ZEND_AST_IS_LIST_SHIFT)) {
        uint32_t lineno = r_u32(f);
        uint32_t children = r_u32(f);
        opdump_guard_u32(children, OPDUMP_MAX_LITERALS, "AST list child");
        zend_ast_list *node = (zend_ast_list *) emalloc(sizeof(zend_ast_list) + (children ? children - 1 : 0) * sizeof(zend_ast *));
        node->kind = (zend_ast_kind) kind;
        node->attr = (zend_ast_attr) attr;
        node->lineno = lineno;
        node->children = children;
        for (uint32_t i = 0; i < children; i++) {
            node->child[i] = opdump_read_ast(f, depth + 1);
        }
        return (zend_ast *) node;
    }

    uint32_t lineno = r_u32(f);
    uint32_t children = r_u32(f);
    opdump_guard_u32(children, OPDUMP_MAX_LITERALS, "AST child");
    zend_ast *node = (zend_ast *) emalloc(sizeof(zend_ast) + (children ? children - 1 : 0) * sizeof(zend_ast *));
    node->kind = (zend_ast_kind) kind;
    node->attr = (zend_ast_attr) attr;
    node->lineno = lineno;
    for (uint32_t i = 0; i < children; i++) {
        node->child[i] = opdump_read_ast(f, depth + 1);
    }
    return node;
}

/* Frees the temporary tree built by opdump_read_ast after zend_ast_copy has
 * taken its own refcounted copy. Mirrors opdump_read_ast's shape decisions and
 * drops the one zval reference each ZVAL/CONSTANT leaf holds. */
static void opdump_free_ast(zend_ast *ast)
{
    if (!ast) {
        return;
    }
    if (ast->kind == ZEND_AST_ZVAL || ast->kind == ZEND_AST_CONSTANT) {
        zval_ptr_dtor_nogc(zend_ast_get_zval(ast));
    } else if (zend_ast_is_list(ast)) {
        zend_ast_list *list = zend_ast_get_list(ast);
        for (uint32_t i = 0; i < list->children; i++) {
            opdump_free_ast(list->child[i]);
        }
    } else {
        uint32_t children = zend_ast_get_num_children(ast);
        for (uint32_t i = 0; i < children; i++) {
            opdump_free_ast(ast->child[i]);
        }
    }
    efree(ast);
}

/* ---- safe variable-name obfuscation (Rung E) ----
 *
 * Compiled opcodes reference CVs (local variables) by integer slot index,
 * never by name -- op_array->vars[] is purely a name-lookup table used by
 * compact()/extract()/get_defined_vars(), `$$name`/`${expr}`, and
 * Reflection/debugging. Renaming entries in that table is a pure metadata
 * edit with zero effect on control flow (unlike opcode reordering, which
 * this project deliberately does not do -- see docs/PHASE4.md). This scan
 * decides, per op_array, whether ANY construct could read a local variable
 * by a name string derived at runtime; if so, that op_array is left
 * untouched rather than risk it silently returning the wrong thing to
 * compact()/extract() -- the same "fail safe, not fail clever" bar as the
 * rest of this loader. It does not chase dynamic dispatch (call_user_func()
 * with a computed callable, a variable holding the string "compact") past
 * one level of indirection; those opcodes (INIT_DYNAMIC_CALL/INIT_USER_CALL)
 * are conservatively treated as unsafe outright rather than attempted. */
static bool opdump_is_dynamic_var_fname(zend_string *fname)
{
    return zend_string_equals_literal_ci(fname, "compact")
        || zend_string_equals_literal_ci(fname, "extract")
        || zend_string_equals_literal_ci(fname, "get_defined_vars");
}

static bool opdump_op_array_uses_dynamic_vars(zend_op_array *op_array)
{
    for (uint32_t i = 0; i < op_array->last; i++) {
        zend_op *opline = &op_array->opcodes[i];
        switch (opline->opcode) {
            case ZEND_INIT_FCALL:
            case ZEND_INIT_FCALL_BY_NAME:
            case ZEND_INIT_NS_FCALL_BY_NAME: {
                if (opline->op2_type != IS_CONST) {
                    return true; /* not a literal function name we can inspect */
                }
                zval *fname_zv = RT_CONSTANT(opline, opline->op2);
                if (Z_TYPE_P(fname_zv) != IS_STRING || opdump_is_dynamic_var_fname(Z_STR_P(fname_zv))) {
                    return true;
                }
                break;
            }
            case ZEND_INIT_DYNAMIC_CALL:
            case ZEND_INIT_USER_CALL:
                return true;
            case ZEND_FETCH_R:
            case ZEND_FETCH_W:
            case ZEND_FETCH_RW:
            case ZEND_FETCH_IS:
            case ZEND_FETCH_FUNC_ARG:
            case ZEND_FETCH_UNSET:
                /* $$name / ${$expr}: the variable's name is itself a runtime
                 * value (IS_VAR/IS_TMP_VAR), not a fixed CV or literal name.
                 * `global $x;` also uses this opcode family but with
                 * op1_type IS_CONST -- it looks up an unrelated global-table
                 * literal, not this op_array's CV table, so it's fine. */
                if (opline->op1_type == IS_VAR || opline->op1_type == IS_TMP_VAR) {
                    return true;
                }
                break;
            default:
                break;
        }
    }
    return false;
}

static void opdump_write_op_array(FILE *f, zend_op_array *op_array)
{
    w_u8(f, op_array->type);
    w_u8(f, op_array->arg_flags[0]);
    w_u8(f, op_array->arg_flags[1]);
    w_u8(f, op_array->arg_flags[2]);
    w_u32(f, op_array->fn_flags);
    w_zstr(f, op_array->function_name);
    w_u32(f, op_array->num_args);
    w_u32(f, op_array->required_num_args);
    w_u32(f, op_array->T);
    w_i32(f, op_array->cache_size);
    w_i32(f, op_array->last_var);
    w_i32(f, op_array->last_try_catch);

    const char *virtual_filename = getenv("OPDUMP_VIRTUAL_FILENAME");
    if (virtual_filename && virtual_filename[0]) {
        w_str(f, virtual_filename, strlen(virtual_filename));
    } else {
        w_str(f, op_array->filename ? ZSTR_VAL(op_array->filename) : "", op_array->filename ? ZSTR_LEN(op_array->filename) : 0);
    }
    w_u32(f, op_array->line_start);
    w_u32(f, op_array->line_end);

    w_u32(f, op_array->last);
    w_u32(f, (uint32_t)op_array->last_literal);
    w_u32(f, op_array->num_dynamic_func_defs);
    for (uint32_t i = 0; i < op_array->last; i++) {
        zend_op *op = &op_array->opcodes[i];
        w_u8(f, op->opcode);
        w_u8(f, op->op1_type);
        w_u8(f, op->op2_type);
        w_u8(f, op->result_type);
        w_u32(f, op->extended_value);
        w_u32(f, op->lineno);
        w_u32(f, op->op1.num);
        w_u32(f, op->op2.num);
        w_u32(f, op->result.num);
    }

    for (int i = 0; i < op_array->last_literal; i++) {
        opdump_write_literal(f, &op_array->literals[i]);
    }
    for (int i = 0; i < op_array->last_try_catch; i++) {
        zend_try_catch_element *elem = &op_array->try_catch_array[i];
        w_u32(f, elem->try_op);
        w_u32(f, elem->catch_op);
        w_u32(f, elem->finally_op);
        w_u32(f, elem->finally_end);
    }
    /* Only non-parameter locals are ever renamed: arg_info[i].name (used for
     * named-argument matching and ReflectionParameter::getName()) is a
     * separate zend_string from the CV table and is never touched here, so
     * renaming a parameter's CV entry would make the two silently diverge. */
    bool obfuscate_vars = getenv("OPDUMP_OBFUSCATE") != NULL
        && !opdump_op_array_uses_dynamic_vars(op_array);
    uint32_t protected_var_count = op_array->num_args + ((op_array->fn_flags & ZEND_ACC_VARIADIC) ? 1 : 0);
    for (int i = 0; i < op_array->last_var; i++) {
        if (obfuscate_vars && (uint32_t) i >= protected_var_count) {
            char renamed[24];
            snprintf(renamed, sizeof(renamed), "_v%d", i);
            w_u8(f, 1);
            w_str(f, renamed, strlen(renamed));
        } else {
            w_zstr(f, op_array->vars[i]);
        }
    }
    if (op_array->fn_flags & ZEND_ACC_HAS_RETURN_TYPE) {
        opdump_write_arg_info(f, &op_array->arg_info[-1]);
    }
    for (uint32_t i = 0; i < op_array->num_args; i++) {
        opdump_write_arg_info(f, &op_array->arg_info[i]);
    }
    for (uint32_t i = 0; i < op_array->num_dynamic_func_defs; i++) {
        opdump_write_op_array(f, op_array->dynamic_func_defs[i]);
    }

    /* `static $v = <initial>;` inside a function/method. The initial values live
     * in op_array->static_variables keyed by variable name and can be arrays or
     * constant-AST, so they route through opdump_write_literal like any other
     * value. Without this, static locals silently come back as NULL. */
    if (op_array->static_variables) {
        w_u32(f, zend_hash_num_elements(op_array->static_variables));
        zend_string *sv_key;
        zval *sv_val;
        ZEND_HASH_MAP_FOREACH_STR_KEY_VAL(op_array->static_variables, sv_key, sv_val) {
            w_zstr(f, sv_key);
            ZVAL_DEREF(sv_val);
            opdump_write_literal(f, sv_val);
        } ZEND_HASH_FOREACH_END();
    } else {
        w_u32(f, 0);
    }
}

static bool opdump_function_belongs_to_file(zend_function *func, zend_string *filename)
{
    return func->type == ZEND_USER_FUNCTION
        && func->op_array.filename
        && filename
        && zend_string_equals(func->op_array.filename, filename);
}

static bool opdump_class_belongs_to_file(zend_class_entry *ce, zend_string *filename)
{
    return ce->type == ZEND_USER_CLASS
        && ce->info.user.filename
        && filename
        && zend_string_equals(ce->info.user.filename, filename);
}

static uint32_t opdump_count_file_functions(zend_string *filename)
{
    uint32_t count = 0;
    zend_function *func;

    ZEND_HASH_MAP_FOREACH_PTR(CG(function_table), func) {
        if (opdump_function_belongs_to_file(func, filename)) {
            count++;
        }
    } ZEND_HASH_FOREACH_END();

    return count;
}

static void opdump_write_file_functions(FILE *f, zend_string *filename)
{
    zend_string *key;
    zend_function *func;
    uint32_t count = opdump_count_file_functions(filename);

    w_u32(f, count);
    ZEND_HASH_MAP_FOREACH_STR_KEY_PTR(CG(function_table), key, func) {
        if (opdump_function_belongs_to_file(func, filename)) {
            w_zstr(f, key);
            opdump_write_op_array(f, &func->op_array);
        }
    } ZEND_HASH_FOREACH_END();
}

static uint32_t opdump_count_file_classes(zend_string *filename)
{
    uint32_t count = 0;
    zend_class_entry *ce;

    ZEND_HASH_MAP_FOREACH_PTR(CG(class_table), ce) {
        if (opdump_class_belongs_to_file(ce, filename)) {
            count++;
        }
    } ZEND_HASH_FOREACH_END();

    return count;
}

static void opdump_write_property_info(FILE *f, zend_property_info *prop)
{
    w_u32(f, prop->offset);
    w_u32(f, prop->flags);
    w_zstr(f, prop->name);
    w_zstr(f, prop->doc_comment);
    opdump_write_type(f, prop->type);
}

static zend_property_info *opdump_read_property_info(FILE *f, zend_class_entry *ce)
{
    zend_property_info *prop = (zend_property_info *) ecalloc(1, sizeof(zend_property_info));
    prop->offset = r_u32(f);
    prop->flags = r_u32(f);
    prop->name = r_zstr(f, true);
    prop->doc_comment = r_zstr(f, true);
    prop->type = opdump_read_type(f);
    prop->ce = ce;
    prop->prototype = prop;
    return prop;
}

static void opdump_write_class_entry(FILE *f, zend_class_entry *ce)
{
    zend_string *key;
    zend_function *func;
    zend_property_info *prop;
    uint32_t method_count = 0;
    uint32_t prop_count = 0;

    w_zstr(f, ce->name);
    w_u32(f, ce->ce_flags);
    w_i32(f, ce->default_properties_count);
    w_i32(f, ce->default_static_members_count);
    const char *virtual_filename = getenv("OPDUMP_VIRTUAL_FILENAME");
    if (virtual_filename && virtual_filename[0]) {
        w_u8(f, 1);
        w_str(f, virtual_filename, strlen(virtual_filename));
    } else {
        w_zstr(f, ce->info.user.filename);
    }
    w_u32(f, ce->info.user.line_start);
    w_u32(f, ce->info.user.line_end);

    for (int i = 0; i < ce->default_properties_count; i++) {
        opdump_write_literal(f, &ce->default_properties_table[i]);
    }

    ZEND_HASH_MAP_FOREACH_PTR(&ce->properties_info, prop) {
        prop_count++;
    } ZEND_HASH_FOREACH_END();
    w_u32(f, prop_count);
    ZEND_HASH_MAP_FOREACH_STR_KEY_PTR(&ce->properties_info, key, prop) {
        w_zstr(f, key);
        opdump_write_property_info(f, prop);
    } ZEND_HASH_FOREACH_END();

    ZEND_HASH_MAP_FOREACH_STR_KEY_PTR(&ce->function_table, key, func) {
        if (func->type == ZEND_USER_FUNCTION) {
            method_count++;
        }
    } ZEND_HASH_FOREACH_END();
    w_u32(f, method_count);
    ZEND_HASH_MAP_FOREACH_STR_KEY_PTR(&ce->function_table, key, func) {
        if (func->type == ZEND_USER_FUNCTION) {
            w_zstr(f, key);
            opdump_write_op_array(f, &func->op_array);
        }
    } ZEND_HASH_FOREACH_END();

    /* --- class constants (own only; inherited ones arrive via linking) ---
     * A constant's value is an ordinary zval that may be a scalar, an array,
     * or an unevaluated IS_CONSTANT_AST (e.g. `const B = self::A | 4`); the
     * visibility/final bits live in its u2 constant_flags. Without this a
     * `Foo::BAR` fetch finds nothing and fails at runtime. */
    zend_class_constant *cc;
    zend_string *cc_key;
    uint32_t const_count = 0;
    ZEND_HASH_MAP_FOREACH_STR_KEY_PTR(&ce->constants_table, cc_key, cc) {
        (void) cc_key;
        if (cc->ce == ce) {
            if ((ZEND_CLASS_CONST_FLAGS(cc) & ZEND_CLASS_CONST_IS_CASE) || Z_TYPE(cc->value) == IS_OBJECT) {
                php_error_docref(NULL, E_ERROR, "opdump: enum cases are not yet supported by the loader (%s)", ZSTR_VAL(ce->name));
            }
            const_count++;
        }
    } ZEND_HASH_FOREACH_END();
    w_u32(f, const_count);
    ZEND_HASH_MAP_FOREACH_STR_KEY_PTR(&ce->constants_table, cc_key, cc) {
        if (cc->ce != ce) {
            continue;
        }
        w_zstr(f, cc_key);
        w_u32(f, (uint32_t) Z_CONSTANT_FLAGS(cc->value));
        opdump_write_literal(f, &cc->value);
    } ZEND_HASH_FOREACH_END();

    /* --- static property defaults --- (default_static_members_count was
     * written above). Values may again be arrays or constant-AST. */
    for (int i = 0; i < ce->default_static_members_count; i++) {
        zval *sv = &ce->default_static_members_table[i];
        ZVAL_DEREF(sv);
        opdump_write_literal(f, sv);
    }

    /* --- inheritance link targets ---
     * Serialize the names of the parent, interfaces, and traits so that a class
     * whose relatives live in other (autoloaded) files can be linked at load
     * time by its DECLARE_CLASS_DELAYED opcode. When the class was already
     * linked at compile time (a same-file parent), its union holds resolved CE
     * pointers instead of names, so read them from there. */
    bool ce_linked = (ce->ce_flags & ZEND_ACC_LINKED) != 0;
    if (ce_linked) {
        w_zstr(f, ce->parent ? ce->parent->name : NULL);
    } else {
        w_zstr(f, ce->parent_name);
    }
    w_u32(f, ce->num_interfaces);
    for (uint32_t i = 0; i < ce->num_interfaces; i++) {
        if (ce_linked) {
            zend_string *nm = ce->interfaces[i]->name;
            zend_string *lc = zend_string_tolower(nm);
            w_zstr(f, nm);
            w_zstr(f, lc);
            zend_string_release(lc);
        } else {
            w_zstr(f, ce->interface_names[i].name);
            w_zstr(f, ce->interface_names[i].lc_name);
        }
    }
    if (!ce_linked && ce->num_traits > 0) {
        if (ce->trait_aliases || ce->trait_precedences) {
            php_error_docref(NULL, E_ERROR, "opdump: trait adaptations (use ... { ... }) on %s are not yet supported by the loader", ZSTR_VAL(ce->name));
        }
        w_u32(f, ce->num_traits);
        for (uint32_t i = 0; i < ce->num_traits; i++) {
            w_zstr(f, ce->trait_names[i].name);
            w_zstr(f, ce->trait_names[i].lc_name);
        }
    } else {
        w_u32(f, 0);
    }
}

/* Rebuild ce->properties_info_table, the offset-indexed lookup the runtime
 * uses for object property access (and that inheritance touches while merging
 * a parent). The compiler normally builds this at class-declaration time via
 * zend_build_properties_info_table(), but that symbol is not exported from the
 * PHP binary, so we reconstruct an equivalent here. Slots left NULL (e.g. for
 * a linked class's inherited properties) are fine -- the engine falls back to
 * its slow per-slot lookup for those. */
static void opdump_build_properties_info_table(zend_class_entry *ce)
{
    if (ce->default_properties_count == 0) {
        ce->properties_info_table = NULL;
        return;
    }
    size_t bytes = sizeof(zend_property_info *) * (size_t) ce->default_properties_count;
    zend_property_info **table = (zend_property_info **) emalloc(bytes);
    memset(table, 0, bytes);
    ce->properties_info_table = table;

    zend_property_info *prop;
    ZEND_HASH_MAP_FOREACH_PTR(&ce->properties_info, prop) {
        if (!(prop->flags & ZEND_ACC_STATIC)) {
            uint32_t num = OBJ_PROP_TO_NUM(prop->offset);
            if (num < (uint32_t) ce->default_properties_count) {
                table[num] = prop;
            }
        }
    } ZEND_HASH_FOREACH_END();
}

static zend_class_entry *opdump_read_class_entry(FILE *f)
{
    zend_class_entry *ce = (zend_class_entry *) ecalloc(1, sizeof(zend_class_entry));
    zend_string *name = r_zstr(f, true);
    ce->type = ZEND_USER_CLASS;
    ce->name = name;
    ce->refcount = 1;
    ce->ce_flags = r_u32(f);
    ce->default_properties_count = r_i32(f);
    ce->default_static_members_count = r_i32(f);
    opdump_guard_i32_nonnegative(ce->default_properties_count, OPDUMP_MAX_CLASS_PROPERTIES, "default properties");
    opdump_guard_i32_nonnegative(ce->default_static_members_count, OPDUMP_MAX_CLASS_PROPERTIES, "default static members");
#if PHP_VERSION_ID >= 80300
    ce->default_object_handlers = &std_object_handlers;
#endif
    ce->info.user.filename = r_zstr(f, true);
    ce->info.user.line_start = r_u32(f);
    ce->info.user.line_end = r_u32(f);

    zend_hash_init(&ce->function_table, 8, NULL, ZEND_FUNCTION_DTOR, 0);
    zend_hash_init(&ce->properties_info, 8, NULL, NULL, 0);
    zend_hash_init(&ce->constants_table, 0, NULL, NULL, 0);

    if (ce->default_properties_count > 0) {
        ce->default_properties_table = (zval *) ecalloc(ce->default_properties_count, sizeof(zval));
        for (int i = 0; i < ce->default_properties_count; i++) {
            opdump_read_literal(f, &ce->default_properties_table[i]);
        }
    }

    uint32_t prop_count = r_u32(f);
    opdump_guard_u32(prop_count, OPDUMP_MAX_CLASS_PROPERTIES, "class property");
    for (uint32_t i = 0; i < prop_count; i++) {
        zend_string *prop_key = r_zstr(f, true);
        zend_property_info *prop = opdump_read_property_info(f, ce);
        zend_hash_add_ptr(&ce->properties_info, prop_key, prop);
        zend_string_release(prop_key);
    }

    uint32_t method_count = r_u32(f);
    opdump_guard_u32(method_count, OPDUMP_MAX_CLASS_METHODS, "class method");
    for (uint32_t i = 0; i < method_count; i++) {
        zend_string *key = r_zstr(f, true);
        zend_op_array *method = opdump_read_op_array(f);
        method->scope = ce;
        if (method->function_name && zend_string_equals_literal_ci(method->function_name, "__construct")) {
            ce->constructor = (zend_function *) method;
        }
        zend_hash_add_ptr(&ce->function_table, key, method);
        zend_string_release(key);
    }

    /* class constants */
    uint32_t const_count = r_u32(f);
    opdump_guard_u32(const_count, OPDUMP_MAX_CLASS_METHODS, "class constant");
    for (uint32_t i = 0; i < const_count; i++) {
        zend_string *cname = r_zstr(f, true);
        uint32_t cflags = r_u32(f);
        zend_class_constant *cc = (zend_class_constant *) emalloc(sizeof(zend_class_constant));
        memset(cc, 0, sizeof(zend_class_constant));
        opdump_read_literal(f, &cc->value);
        Z_CONSTANT_FLAGS(cc->value) = cflags;
        cc->ce = ce;
        cc->type = (zend_type) ZEND_TYPE_INIT_NONE(0);
        zend_hash_add_ptr(&ce->constants_table, cname, cc);
        zend_string_release(cname);
    }

    /* static property defaults + the MAP_PTR slot the runtime reads through */
    if (ce->default_static_members_count > 0) {
        ce->default_static_members_table = (zval *) ecalloc(ce->default_static_members_count, sizeof(zval));
        for (int i = 0; i < ce->default_static_members_count; i++) {
            opdump_read_literal(f, &ce->default_static_members_table[i]);
        }
        ZEND_MAP_PTR_NEW(ce->static_members_table);
    } else {
        ZEND_MAP_PTR_INIT(ce->static_members_table, NULL);
    }

    /* inheritance link targets (parent, interfaces, traits) */
    zend_string *parent_name = r_zstr(f, true);
    uint32_t num_ifaces = r_u32(f);
    opdump_guard_u32(num_ifaces, OPDUMP_MAX_CLASS_METHODS, "class interface");
    zend_class_name *iface_names = NULL;
    if (num_ifaces > 0) {
        iface_names = (zend_class_name *) emalloc(sizeof(zend_class_name) * num_ifaces);
        for (uint32_t i = 0; i < num_ifaces; i++) {
            iface_names[i].name = r_zstr(f, true);
            iface_names[i].lc_name = r_zstr(f, true);
        }
    }
    uint32_t num_tr = r_u32(f);
    opdump_guard_u32(num_tr, OPDUMP_MAX_CLASS_METHODS, "class trait");
    zend_class_name *tr_names = NULL;
    if (num_tr > 0) {
        tr_names = (zend_class_name *) emalloc(sizeof(zend_class_name) * num_tr);
        for (uint32_t i = 0; i < num_tr; i++) {
            tr_names[i].name = r_zstr(f, true);
            tr_names[i].lc_name = r_zstr(f, true);
        }
    }

    /* Only an unlinked class still needs linking: install the recovered names
     * into the union fields the linker consumes. An already-linked class was
     * captured with its inheritance flattened in, so we keep that and drop the
     * recorded names. */
    if (!(ce->ce_flags & ZEND_ACC_LINKED)) {
        if (parent_name) {
            ce->parent_name = parent_name;
            parent_name = NULL;
        }
        if (num_ifaces > 0) {
            ce->num_interfaces = num_ifaces;
            ce->interface_names = iface_names;
            iface_names = NULL;
        }
        if (num_tr > 0) {
            ce->num_traits = num_tr;
            ce->trait_names = tr_names;
            tr_names = NULL;
        }
    }
    if (parent_name) {
        zend_string_release(parent_name);
    }
    if (iface_names) {
        for (uint32_t i = 0; i < num_ifaces; i++) {
            zend_string_release(iface_names[i].name);
            zend_string_release(iface_names[i].lc_name);
        }
        efree(iface_names);
    }
    if (tr_names) {
        for (uint32_t i = 0; i < num_tr; i++) {
            zend_string_release(tr_names[i].name);
            zend_string_release(tr_names[i].lc_name);
        }
        efree(tr_names);
    }

    /* Rebuild the offset->property_info lookup table the runtime uses for
     * object property access and that inheritance reads while merging a parent
     * -- the compiler builds this at class-declaration time, so a loaded class
     * must too or property access (and delayed linking) dereferences NULL. For
     * an unlinked class this covers its own properties; the linker rebuilds it
     * to include inherited ones when DECLARE_CLASS_DELAYED fires. */
    opdump_build_properties_info_table(ce);

    return ce;
}

static void opdump_write_file_classes(FILE *f, zend_string *filename)
{
    zend_string *key;
    zend_class_entry *ce;
    uint32_t count = opdump_count_file_classes(filename);

    w_u32(f, count);
    ZEND_HASH_MAP_FOREACH_STR_KEY_PTR(CG(class_table), key, ce) {
        if (opdump_class_belongs_to_file(ce, filename)) {
            w_zstr(f, key);
            opdump_write_class_entry(f, ce);
        }
    } ZEND_HASH_FOREACH_END();
}

static void opdump_write(zend_op_array *op_array, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        php_error_docref(NULL, E_WARNING, "opdump: cannot open %s for writing", path);
        return;
    }
    fwrite(OPDUMP_MAGIC, 1, 4, f);
    w_u32(f, PHP_VERSION_ID);
    opdump_write_op_array(f, op_array);
    opdump_write_file_functions(f, op_array->filename);
    opdump_write_file_classes(f, op_array->filename);

    fclose(f);
    if (getenv("OPDUMP_DEBUG")) {
        php_error_docref(NULL, E_NOTICE, "opdump: wrote %s (%u opcodes, %d literals, %u functions, %u classes)",
            path, op_array->last, op_array->last_literal,
            opdump_count_file_functions(op_array->filename),
            opdump_count_file_classes(op_array->filename));
    }
}

static zend_op_array *opdump_dump_compile_file(zend_file_handle *file_handle, int type)
{
    zend_op_array *op_array = opdump_orig_compile_file(file_handle, type);
    if (op_array) {
        const char *out = getenv("OPDUMP_OUT");
        if (out) {
            opdump_write(op_array, out);
        }
    }
    return op_array;
}

/* ---- load ---- */

static zend_op_array *opdump_read_op_array(FILE *f)
{
    uint8_t saved_type = r_u8(f);
    uint8_t saved_arg_flags[3];
    saved_arg_flags[0] = r_u8(f);
    saved_arg_flags[1] = r_u8(f);
    saved_arg_flags[2] = r_u8(f);
    uint32_t saved_fn_flags = r_u32(f);
    zend_string *function_name = r_zstr(f, true);
    uint32_t saved_num_args = r_u32(f);
    uint32_t saved_required_num_args = r_u32(f);
    uint32_t saved_T = r_u32(f);
    int32_t saved_cache_size = r_i32(f);
    int32_t saved_last_var = r_i32(f);
    int32_t saved_last_try_catch = r_i32(f);
    opdump_guard_u32(saved_num_args, OPDUMP_MAX_ARGS, "argument");
    if (saved_required_num_args > saved_num_args) {
        php_error_docref(NULL, E_ERROR, "opdump: required_num_args %u exceeds num_args %u", saved_required_num_args, saved_num_args);
    }
    opdump_guard_u32(saved_T, OPDUMP_MAX_VARS, "temporary variable");
    opdump_guard_i32_nonnegative(saved_cache_size, UINT32_MAX, "runtime cache size");
    opdump_guard_i32_nonnegative(saved_last_var, OPDUMP_MAX_VARS, "compiled variable");
    opdump_guard_i32_nonnegative(saved_last_try_catch, OPDUMP_MAX_TRY_CATCH, "try/catch");
#if PHP_VERSION_ID < 80200
    if (saved_cache_size == 0 && (saved_fn_flags & ZEND_ACC_HAS_TYPE_HINTS)) {
        saved_cache_size = (int32_t) sizeof(void *);
    }
#endif

    uint32_t fname_len;
    char *fname = r_str(f, &fname_len);
    zend_string *fname_zs = zend_string_init(fname, fname_len, 0);
    free(fname);

    /* init_op_array() unconditionally does
     * filename = zend_string_copy(zend_get_compiled_filename()),
     * i.e. it reads CG(compiled_filename) -- a compiler-global normally
     * populated by the real zend_compile_file() before it ever calls
     * init_op_array(). Since this hook bypasses that entirely, the global
     * is NULL here unless we set it ourselves first (skipping this step
     * segfaults inside zend_string_copy(NULL)). */
    zend_string *prev_compiled_filename = zend_set_compiled_filename(fname_zs);

    zend_op_array *op_array = (zend_op_array *) emalloc(sizeof(zend_op_array));
    init_op_array(op_array, ZEND_USER_FUNCTION, 1);

    zend_restore_compiled_filename(prev_compiled_filename);
    zend_string_release(fname_zs);

    op_array->type     = saved_type;
    op_array->arg_flags[0] = saved_arg_flags[0];
    op_array->arg_flags[1] = saved_arg_flags[1];
    op_array->arg_flags[2] = saved_arg_flags[2];
    op_array->fn_flags = saved_fn_flags;
    op_array->function_name = function_name;
    op_array->num_args = saved_num_args;
    op_array->required_num_args = saved_required_num_args;
    op_array->T        = saved_T;
    op_array->cache_size = saved_cache_size;
    op_array->last_var = saved_last_var;
    op_array->last_try_catch = saved_last_try_catch;
    op_array->line_start = r_u32(f);
    op_array->line_end   = r_u32(f);

    uint32_t last = r_u32(f);
    uint32_t last_literal_u32 = r_u32(f);
    uint32_t num_dynamic_func_defs = r_u32(f);
    opdump_guard_u32(last, OPDUMP_MAX_OPCODES, "opcode");
    opdump_guard_u32(last_literal_u32, OPDUMP_MAX_LITERALS, "literal");
    opdump_guard_u32(num_dynamic_func_defs, OPDUMP_MAX_DYNAMIC_FUNCS, "dynamic function");
    int last_literal = (int) last_literal_u32;

    /* Constant operands are NOT stored as offsets into op_array->literals.
     * On 64-bit builds (ZEND_USE_ABS_CONST_ADDR == 0), RT_CONSTANT() computes
     * a literal's address as (char*)opline + op1.constant -- i.e. an offset
     * relative to *that specific opline's own address*. This only resolves
     * correctly if opcodes and literals live in one contiguous block laid
     * out exactly the way pass_two() (Zend/zend_opcode.c) does it: literals
     * start right after the (16-byte-aligned) opcodes array. Two separate
     * ecalloc() calls -- what an earlier version of this function did --
     * silently produces garbage constant reads, since the offsets baked in
     * at dump time were computed against the original compiler's layout. */
    if (last > SIZE_MAX / sizeof(zend_op) || (uint32_t)last_literal > SIZE_MAX / sizeof(zval)) {
        php_error_docref(NULL, E_ERROR, "opdump: opcode/literal allocation size overflow");
    }
    size_t ops_size = ZEND_MM_ALIGNED_SIZE_EX(sizeof(zend_op) * (size_t)last, 16);
    size_t literal_size = sizeof(zval) * (size_t)last_literal;
    if (ops_size > SIZE_MAX - literal_size) {
        php_error_docref(NULL, E_ERROR, "opdump: opcode/literal allocation size overflow");
    }
    size_t total_size = ops_size + literal_size;
    efree(op_array->opcodes);
    char *block = (char *) ecalloc(1, total_size ? total_size : 1);
    op_array->opcodes = (zend_op *) block;
    op_array->literals = last_literal ? (zval *)(block + ops_size) : NULL;
    op_array->last = last;

    for (uint32_t i = 0; i < last; i++) {
        zend_op *op = &op_array->opcodes[i];
        op->opcode      = r_u8(f);
        op->op1_type    = r_u8(f);
        op->op2_type    = r_u8(f);
        op->result_type = r_u8(f);
        op->extended_value = r_u32(f);
        op->lineno      = r_u32(f);
        op->op1.num     = r_u32(f);
        op->op2.num     = r_u32(f);
        op->result.num  = r_u32(f);
    }
    for (uint32_t i = 0; i < last; i++) {
        zend_vm_set_opcode_handler(&op_array->opcodes[i]);
    }

    op_array->last_literal = last_literal;
    for (int i = 0; i < last_literal; i++) {
        opdump_read_literal(f, &op_array->literals[i]);
    }

    if (op_array->last_try_catch > 0) {
        op_array->try_catch_array = (zend_try_catch_element *) ecalloc(op_array->last_try_catch, sizeof(zend_try_catch_element));
        for (int i = 0; i < op_array->last_try_catch; i++) {
            zend_try_catch_element *elem = &op_array->try_catch_array[i];
            elem->try_op = r_u32(f);
            elem->catch_op = r_u32(f);
            elem->finally_op = r_u32(f);
            elem->finally_end = r_u32(f);
        }
    }

    if (op_array->last_var > 0) {
        op_array->vars = (zend_string **) ecalloc(op_array->last_var, sizeof(zend_string *));
        for (int i = 0; i < op_array->last_var; i++) {
            op_array->vars[i] = r_zstr(f, true);
        }
    }

    if (op_array->num_args > 0 || (op_array->fn_flags & ZEND_ACC_HAS_RETURN_TYPE)) {
        uint32_t return_slots = (op_array->fn_flags & ZEND_ACC_HAS_RETURN_TYPE) ? 1 : 0;
        zend_arg_info *arg_info_base = (zend_arg_info *) ecalloc(op_array->num_args + return_slots, sizeof(zend_arg_info));
        op_array->arg_info = arg_info_base + return_slots;
        if (return_slots) {
            opdump_read_arg_info(f, &op_array->arg_info[-1]);
        }
        for (uint32_t i = 0; i < op_array->num_args; i++) {
            opdump_read_arg_info(f, &op_array->arg_info[i]);
        }
    }

    op_array->num_dynamic_func_defs = num_dynamic_func_defs;
    if (num_dynamic_func_defs > 0) {
        op_array->dynamic_func_defs = (zend_op_array **) ecalloc(num_dynamic_func_defs, sizeof(zend_op_array *));
        for (uint32_t i = 0; i < num_dynamic_func_defs; i++) {
            op_array->dynamic_func_defs[i] = opdump_read_op_array(f);
        }
    }

    /* Rebuild the `static $v = ...` initializer table and point the op_array's
     * MAP_PTR slot at it, exactly as the compiler leaves a freshly-parsed
     * op_array: ZEND_BIND_STATIC reads static_variables_ptr at runtime and
     * duplicates this template into per-invocation storage on first use. */
    uint32_t static_var_count = r_u32(f);
    opdump_guard_u32(static_var_count, OPDUMP_MAX_LITERALS, "static variable");
    if (static_var_count > 0) {
        HashTable *statics;
        ALLOC_HASHTABLE(statics);
        zend_hash_init(statics, static_var_count, NULL, ZVAL_PTR_DTOR, 0);
        for (uint32_t i = 0; i < static_var_count; i++) {
            zend_string *sv_key = r_zstr(f, false);
            zval sv_val;
            opdump_read_literal(f, &sv_val);
            zend_hash_update(statics, sv_key, &sv_val);
            zend_string_release(sv_key);
        }
        op_array->static_variables = statics;
    } else {
        op_array->static_variables = NULL;
    }
#if PHP_VERSION_ID < 80200
    ZEND_MAP_PTR_INIT(op_array->static_variables_ptr, &op_array->static_variables);
#else
    ZEND_MAP_PTR_INIT(op_array->static_variables_ptr, op_array->static_variables);
#endif

    opdump_init_run_time_cache(op_array);

    if (getenv("OPDUMP_DEBUG")) {
        fprintf(stderr, "[opdump debug] op_array name=%s fn_flags=%u cache_size=%d num_args=%u required=%u T=%u last_var=%d runtime_cache=%p\n",
            op_array->function_name ? ZSTR_VAL(op_array->function_name) : "{main}",
            op_array->fn_flags, op_array->cache_size, op_array->num_args,
            op_array->required_num_args, op_array->T, op_array->last_var,
            op_array->cache_size > 0 ? (void*) RUN_TIME_CACHE(op_array) : NULL);
        for (uint32_t i = 0; i < op_array->last; i++) {
            zend_op *op = &op_array->opcodes[i];
            fprintf(stderr, "[opdump debug] op[%u]: opcode=%u op1_type=%u op2_type=%u result_type=%u ext=%u op1.num=%u op2.num=%u result.num=%u handler=%p\n",
                i, op->opcode, op->op1_type, op->op2_type, op->result_type,
                op->extended_value, op->op1.num, op->op2.num, op->result.num, op->handler);
        }
        for (int i = 0; i < op_array->last_literal; i++) {
            zval *zv = &op_array->literals[i];
            fprintf(stderr, "[opdump debug] literal[%d]: type=%d", i, Z_TYPE_P(zv));
            if (Z_TYPE_P(zv) == IS_STRING) fprintf(stderr, " value=\"%s\"", Z_STRVAL_P(zv));
            if (Z_TYPE_P(zv) == IS_LONG) fprintf(stderr, " value=%ld", (long)Z_LVAL_P(zv));
            fprintf(stderr, "\n");
        }
        for (uint32_t i = 0; i < op_array->num_args; i++) {
            zend_arg_info *arg_info = &op_array->arg_info[i];
            fprintf(stderr, "[opdump debug] arg[%u]: name=%s type_mask=%u default=%p\n",
                i,
                arg_info->name ? ZSTR_VAL(arg_info->name) : "{null}",
                ZEND_TYPE_FULL_MASK(arg_info->type),
                (void*) arg_info->default_value);
        }
        fprintf(stderr, "[opdump debug] literals base ptr = %p\n", (void*)op_array->literals);
    }
    return op_array;
}

static zend_op_array *opdump_read_raw_stream(FILE *f, const char *path)
{
    char magic[4];
    r_exact(f, magic, sizeof(magic), "backend magic");
    if (memcmp(magic, OPDUMP_MAGIC, 4) != 0) {
        php_error_docref(NULL, E_ERROR, "opdump: bad backend magic in %s", path);
        return NULL;
    }
    uint32_t built_for_version = r_u32(f);
    if (built_for_version != PHP_VERSION_ID) {
        php_error_docref(NULL, E_WARNING,
            "opdump: blob was dumped under PHP_VERSION_ID %u, running %u -- proceeding anyway for this spike",
            built_for_version, PHP_VERSION_ID);
    }

    zend_op_array *op_array = opdump_read_op_array(f);
    uint32_t function_count = r_u32(f);
    opdump_guard_u32(function_count, OPDUMP_MAX_FILE_FUNCTIONS, "file function");
    for (uint32_t i = 0; i < function_count; i++) {
        zend_string *key = r_zstr(f, true);
        zend_op_array *func_op_array = opdump_read_op_array(f);
        zend_hash_add_ptr(EG(function_table), key, func_op_array);
        zend_string_release(key);
    }
    uint32_t class_count = r_u32(f);
    opdump_guard_u32(class_count, OPDUMP_MAX_FILE_CLASSES, "file class");
    for (uint32_t i = 0; i < class_count; i++) {
        zend_string *key = r_zstr(f, true);
        zend_class_entry *ce = opdump_read_class_entry(f);
        zend_hash_add_ptr(EG(class_table), key, ce);
        zend_string *lcname = zend_string_tolower(ce->name);
        /* A class whose parent/interfaces were not yet known when its file was
         * compiled is stored under a "delayed early binding" key -- a synthetic
         * name with a leading NUL byte -- and the file's op_array carries a
         * DECLARE_CLASS_DELAYED opcode that links it (autoloading the parent)
         * and installs it under its real lowercased name at runtime. If we also
         * pre-registered it under that real name here, the later delayed bind
         * would see the name already taken and abort with "Cannot redeclare
         * class" (or leave it unlinked so parent methods look missing). So only
         * alias to the lowercased name for normal (non-delayed) keys; delayed
         * ones get aliased for us when their DECLARE_CLASS_DELAYED runs. */
        bool delayed_binding_key = ZSTR_LEN(key) > 0 && ZSTR_VAL(key)[0] == '\0';
        if (!delayed_binding_key && !zend_string_equals(lcname, key)) {
            zend_hash_add_ptr(EG(class_table), lcname, ce);
        }
        if (getenv("OPDUMP_DEBUG")) {
            fprintf(stderr, "[opdump debug] registered class key=\"%s\" lc=\"%s\" name=\"%s\" delayed=%d\n",
                ZSTR_VAL(key), ZSTR_VAL(lcname), ZSTR_VAL(ce->name), (int) delayed_binding_key);
            fprintf(stderr, "[opdump debug] class table has lc? %s\n",
                zend_hash_exists(EG(class_table), lcname) ? "yes" : "no");
        }
        zend_string_release(lcname);
        zend_string_release(key);
    }

    opdump_bind_fcall_caches(op_array);
    zend_function *func;
    ZEND_HASH_MAP_FOREACH_PTR(EG(function_table), func) {
        if (opdump_function_belongs_to_file(func, op_array->filename)) {
            opdump_bind_fcall_caches(&func->op_array);
        }
    } ZEND_HASH_FOREACH_END();

    if (getenv("OPDUMP_DEBUG")) {
        php_error_docref(NULL, E_NOTICE, "opdump: loaded %s (%u opcodes, %d literals, %u dynamic funcs, %u functions, %u classes) without parsing source",
            path, op_array->last, op_array->last_literal, op_array->num_dynamic_func_defs, function_count, class_count);
    }
    return op_array;
}

static zend_op_array *opdump_read(const char *path)
{
    size_t blob_len = 0;
    unsigned char *blob = opdump_read_file_bytes(path, &blob_len);
    if (!blob) {
        php_error_docref(NULL, E_ERROR, "opdump: cannot read %s", path);
        return NULL;
    }
    if (blob_len < 4) {
        free(blob);
        php_error_docref(NULL, E_ERROR, "opdump: blob too short: %s", path);
        return NULL;
    }

    unsigned char *payload = blob;
    size_t payload_len = blob_len;
    bool payload_owned = false;

    if (memcmp(blob, "BYTC", 4) == 0) {
        const char *previous_container_path = opdump_current_container_path;
        opdump_current_container_path = path;
        bool decrypted = opdump_decrypt_bytc(blob, blob_len, &payload, &payload_len);
        opdump_current_container_path = previous_container_path;
        if (!decrypted) {
            free(blob);
            return NULL;
        }
        payload_owned = true;
    } else if (memcmp(blob, OPDUMP_MAGIC, 4) != 0) {
        free(blob);
        php_error_docref(NULL, E_ERROR, "opdump: unknown blob magic in %s", path);
        return NULL;
    }

    FILE *f = fmemopen(payload, payload_len, "rb");
    if (!f) {
        if (payload_owned) free(payload);
        free(blob);
        php_error_docref(NULL, E_ERROR, "opdump: cannot open blob memory stream");
        return NULL;
    }
    zend_op_array *op_array = opdump_read_raw_stream(f, path);
    fclose(f);

    if (payload_owned) {
        free(payload);
    }
    free(blob);
    return op_array;
}

static zend_op_array *opdump_load_compile_file(zend_file_handle *file_handle, int type)
{
    (void) file_handle;
    (void) type;
    const char *in = getenv("OPDUMP_IN");
    if (!in) {
        php_error_docref(NULL, E_ERROR, "opdump: OPDUMP_IN not set in load mode");
        return NULL;
    }
    return opdump_read(in);
}

static zend_op_array *opdump_load_tree_compile_file(zend_file_handle *file_handle, int type)
{
    const char *encoded = opdump_find_tree_blob(file_handle);
    if (encoded) {
        if (getenv("OPDUMP_DEBUG")) {
            const char *filename = opdump_requested_filename(file_handle);
            fprintf(stderr, "[opdump debug] load-tree %s -> %s\n", filename ? filename : "(unknown)", encoded);
        }
        return opdump_read(encoded);
    }
    return opdump_orig_compile_file(file_handle, type);
}

static bool opdump_file_starts_with_loader_blob(const char *path)
{
    unsigned char magic[4];
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    size_t n = fread(magic, 1, sizeof(magic), f);
    fclose(f);
    return n == sizeof(magic)
        && (memcmp(magic, "BYTC", sizeof(magic)) == 0 || memcmp(magic, OPDUMP_MAGIC, sizeof(magic)) == 0);
}

static zend_op_array *opdump_auto_compile_file(zend_file_handle *file_handle, int type)
{
    const char *filename = opdump_requested_filename(file_handle);
    char resolved[PATH_MAX];
    const char *path = filename;

    if (filename && realpath(filename, resolved)) {
        path = resolved;
    }
    if (path && opdump_file_starts_with_loader_blob(path)) {
        if (getenv("OPDUMP_DEBUG")) {
            fprintf(stderr, "[opdump debug] auto-load %s\n", path);
        }
        return opdump_read(path);
    }
    return opdump_orig_compile_file(file_handle, type);
}

static int opdump_startup_hooks(void)
{
    if (opdump_hooks_started) {
        return SUCCESS;
    }
    opdump_disable_core_dumps();
    opdump_orig_compile_file = zend_compile_file;
    const char *mode = getenv("OPDUMP_MODE");
    if (mode && strcmp(mode, "dump") == 0) {
        zend_compile_file = opdump_dump_compile_file;
    } else if (mode && strcmp(mode, "load") == 0) {
        zend_compile_file = opdump_load_compile_file;
    } else if (mode && strcmp(mode, "load-tree") == 0) {
        const char *map = getenv("OPDUMP_MAP");
        if (!map || !map[0]) {
            map = getenv("BYTECODE_MAP");
        }
        if (!map || !map[0]) {
            php_error_docref(NULL, E_WARNING, "opdump: OPDUMP_MAP/BYTECODE_MAP not set in load-tree mode");
        } else if (opdump_load_tree_map(map)) {
            zend_compile_file = opdump_load_tree_compile_file;
        }
    } else {
        zend_compile_file = opdump_auto_compile_file;
    }
    opdump_hooks_started = true;
    return SUCCESS;
}

static void opdump_shutdown_hooks(void)
{
    if (opdump_hooks_started && opdump_orig_compile_file) {
        zend_compile_file = opdump_orig_compile_file;
        opdump_orig_compile_file = NULL;
        opdump_hooks_started = false;
    }
}

/* Cleanses the cached license DEK (if license mode ever unwrapped one) once
 * per process, rather than immediately after each derivation -- it has to
 * survive for the whole request/process to avoid re-doing an RSA-OAEP
 * unwrap per file in load-tree mode (see opdump_license_resolve_ikm). */
static void opdump_clean_license_cache(void)
{
    if (opdump_license_ikm_ready) {
        opdump_unlock_mem(opdump_license_ikm_cache, sizeof(opdump_license_ikm_cache));
        OPENSSL_cleanse(opdump_license_ikm_cache, sizeof(opdump_license_ikm_cache));
        opdump_license_ikm_ready = false;
    }
}

/* ---- module lifecycle ---- */

PHP_MINIT_FUNCTION(opdump)
{
    return opdump_startup_hooks();
}

PHP_MSHUTDOWN_FUNCTION(opdump)
{
    opdump_shutdown_hooks();
    opdump_clean_license_cache();
    return SUCCESS;
}

zend_module_entry opdump_module_entry = {
    STANDARD_MODULE_HEADER,
    "opdump",
    NULL,
    PHP_MINIT(opdump),
    PHP_MSHUTDOWN(opdump),
    NULL, NULL, NULL,
    OPDUMP_VERSION,
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_OPDUMP
ZEND_GET_MODULE(opdump)
#endif

/* Zend-extension entry point. Loading the same .so with
 * `zend_extension=opdump.so` makes PHP list it beside OPcache/ionCube in
 * `php -v`, while reusing the same compile-file hook implementation above. */
static int opdump_zend_startup(zend_extension *extension)
{
    (void) extension;
    return opdump_startup_hooks();
}

static void opdump_zend_shutdown(zend_extension *extension)
{
    (void) extension;
    opdump_shutdown_hooks();
    opdump_clean_license_cache();
}

ZEND_EXTENSION();

ZEND_EXT_API zend_extension zend_extension_entry = {
    .name = "Bytecode PHP Loader",
    .version = OPDUMP_VERSION,
    .author = "MegaMind Technologies LTD",
    .URL = "https://megamindtechnologies.com",
    .copyright = "Copyright (c) 2026 MegaMind Technologies LTD",
    .startup = opdump_zend_startup,
    .shutdown = opdump_zend_shutdown,
    .resource_number = -1
};
