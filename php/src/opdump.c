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
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/crypto.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
# include <direct.h>
# include <windows.h>
# ifndef PATH_MAX
#  define PATH_MAX MAX_PATH
# endif
# define realpath(path, resolved) _fullpath((resolved), (path), PATH_MAX)
#else
# include <sys/resource.h>
# include <sys/mman.h>
#endif

#ifndef OPDUMP_VENDOR_SECRET_HEX
# define OPDUMP_VENDOR_SECRET_HEX ""
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
    OPDUMP_LIT_UNDEF = 6
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
    const char *hex = OPDUMP_VENDOR_SECRET_HEX;
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

static bool opdump_load_tree_map(const char *path)
{
    char *map_dir = opdump_dirname_dup(path);
    if (!opdump_verify_manifest_signature(map_dir, path)) {
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

    char *machine_id = opdump_json_extract_string(json, "machine_id");
    if (machine_id && machine_id[0]) {
        const char *actual_machine_id = getenv("OPDUMP_MACHINE_ID");
        if (!actual_machine_id || strcmp(machine_id, actual_machine_id) != 0) {
            php_error_docref(NULL, E_WARNING, "opdump: license machine_id does not match this server");
            free(machine_id);
            free(json);
            free(derived_path);
            return false;
        }
    }
    free(machine_id);

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
        default:
            php_error_docref(NULL, E_WARNING,
                "opdump: literal type %d not supported yet (Phase 0 scope), writing NULL placeholder",
                Z_TYPE_P(zv));
            w_u8(f, OPDUMP_LIT_NULL);
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
        default:
            ZVAL_NULL(zv);
    }
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

    w_str(f, op_array->filename ? ZSTR_VAL(op_array->filename) : "", op_array->filename ? ZSTR_LEN(op_array->filename) : 0);
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
    w_zstr(f, ce->info.user.filename);
    w_u32(f, ce->info.user.line_start);
    w_u32(f, ce->info.user.line_end);

    for (int i = 0; i < ce->default_properties_count; i++) {
        opdump_write_literal(f, &ce->default_properties_table[i]);
    }

    ZEND_HASH_MAP_FOREACH_PTR(&ce->properties_info, prop) {
        prop_count++;
    } ZEND_HASH_FOREACH_END();
    w_u32(f, prop_count);
    ZEND_HASH_MAP_FOREACH_PTR(&ce->properties_info, prop) {
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
        zend_property_info *prop = opdump_read_property_info(f, ce);
        zend_hash_add_ptr(&ce->properties_info, prop->name, prop);
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
        if (!zend_string_equals(lcname, key)) {
            zend_hash_add_ptr(EG(class_table), lcname, ce);
        }
        if (getenv("OPDUMP_DEBUG")) {
            fprintf(stderr, "[opdump debug] registered class key=\"%s\" lc=\"%s\" name=\"%s\"\n",
                ZSTR_VAL(key), ZSTR_VAL(lcname), ZSTR_VAL(ce->name));
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
    "0.0.1-phase0",
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
    .version = "0.0.1-phase0",
    .author = "MegaMind Technologies LTD",
    .URL = "https://github.com/Mastermegamind/bytecode",
    .copyright = "Copyright (c) 2026 MegaMind Technologies LTD",
    .startup = opdump_zend_startup,
    .shutdown = opdump_zend_shutdown,
    .resource_number = -1
};
