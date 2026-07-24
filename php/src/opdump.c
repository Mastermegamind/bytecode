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
#include "zend_vm.h"
#include "zend_string.h"
#include <openssl/evp.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static zend_op_array *(*opdump_orig_compile_file)(zend_file_handle *file_handle, int type);

typedef struct _opdump_map_entry {
    char *source;
    char *encoded;
    struct _opdump_map_entry *next;
} opdump_map_entry;

static opdump_map_entry *opdump_tree_map = NULL;

/* ---- binary format helpers ---- */

#define OPDUMP_MAGIC "OPD2"

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

static uint8_t  r_u8(FILE *f)  { uint8_t v = 0; size_t n = fread(&v, 1, 1, f); (void)n; return v; }
static uint32_t r_u32(FILE *f) { uint32_t v = 0; size_t n = fread(&v, sizeof(v), 1, f); (void)n; return v; }
static uint64_t r_u64(FILE *f) { uint64_t v = 0; size_t n = fread(&v, sizeof(v), 1, f); (void)n; return v; }
static int32_t  r_i32(FILE *f) { int32_t v = 0; size_t n = fread(&v, sizeof(v), 1, f); (void)n; return v; }
static double   r_dbl(FILE *f) { double v = 0; size_t n = fread(&v, sizeof(v), 1, f); (void)n; return v; }
/* caller frees */
static char *r_str(FILE *f, uint32_t *out_len) {
    uint32_t len = r_u32(f);
    char *buf = malloc(len + 1);
    if (len) { size_t n = fread(buf, 1, len, f); (void)n; }
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

static char *opdump_dirname_dup(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash) {
        return strdup(".");
    }
    if (slash == path) {
        return strdup("/");
    }
    return opdump_strdup_range(path, (size_t)(slash - path));
}

static char *opdump_join_path(const char *base, const char *path)
{
    if (!path || path[0] == '/') {
        return path ? strdup(path) : NULL;
    }
    size_t base_len = strlen(base);
    size_t path_len = strlen(path);
    bool need_slash = base_len > 0 && base[base_len - 1] != '/';
    char *out = (char *) malloc(base_len + (need_slash ? 1 : 0) + path_len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, base, base_len);
    if (need_slash) {
        out[base_len++] = '/';
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

static bool opdump_load_tree_map(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        php_error_docref(NULL, E_WARNING, "opdump: cannot open OPDUMP_MAP %s", path);
        return false;
    }

    char *map_dir = opdump_dirname_dup(path);
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

    size_t aad_len = off;
    if (container_version != 1) {
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
    if (!opdump_key_from_env(key)) {
        php_error_docref(NULL, E_ERROR, "opdump: BYTECODE_KEY/OPDUMP_KEY must be 64 hex chars for BYTC load");
        return false;
    }

    unsigned char *plain = (unsigned char *) malloc(ciphertext_len ? ciphertext_len : 1);
    if (!plain) {
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
            ZVAL_STR(zv, zs);
            break;
        }
        default:
            ZVAL_NULL(zv);
    }
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
    for (int i = 0; i < op_array->last_var; i++) {
        w_zstr(f, op_array->vars[i]);
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
    ce->default_object_handlers = &std_object_handlers;
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
    for (uint32_t i = 0; i < prop_count; i++) {
        zend_property_info *prop = opdump_read_property_info(f, ce);
        zend_hash_add_ptr(&ce->properties_info, prop->name, prop);
    }

    uint32_t method_count = r_u32(f);
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
    php_error_docref(NULL, E_NOTICE, "opdump: wrote %s (%u opcodes, %d literals, %u functions, %u classes)",
        path, op_array->last, op_array->last_literal,
        opdump_count_file_functions(op_array->filename),
        opdump_count_file_classes(op_array->filename));
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
    int last_literal = (int) r_u32(f);
    uint32_t num_dynamic_func_defs = r_u32(f);

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
    size_t ops_size = ZEND_MM_ALIGNED_SIZE_EX(sizeof(zend_op) * last, 16);
    size_t total_size = ops_size + sizeof(zval) * (size_t)last_literal;
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

    if (getenv("OPDUMP_DEBUG")) {
        for (uint32_t i = 0; i < op_array->last; i++) {
            zend_op *op = &op_array->opcodes[i];
            fprintf(stderr, "[opdump debug] op[%u]: opcode=%u op1_type=%u op2_type=%u result_type=%u op1.num=%u op2.num=%u handler=%p\n",
                i, op->opcode, op->op1_type, op->op2_type, op->result_type, op->op1.num, op->op2.num, op->handler);
        }
        for (int i = 0; i < op_array->last_literal; i++) {
            zval *zv = &op_array->literals[i];
            fprintf(stderr, "[opdump debug] literal[%d]: type=%d", i, Z_TYPE_P(zv));
            if (Z_TYPE_P(zv) == IS_STRING) fprintf(stderr, " value=\"%s\"", Z_STRVAL_P(zv));
            if (Z_TYPE_P(zv) == IS_LONG) fprintf(stderr, " value=%ld", (long)Z_LVAL_P(zv));
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "[opdump debug] literals base ptr = %p\n", (void*)op_array->literals);
    }
    return op_array;
}

static zend_op_array *opdump_read_raw_stream(FILE *f, const char *path)
{
    char magic[4];
    size_t n = fread(magic, 1, 4, f); (void)n;
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
    for (uint32_t i = 0; i < function_count; i++) {
        zend_string *key = r_zstr(f, true);
        zend_op_array *func_op_array = opdump_read_op_array(f);
        zend_hash_add_ptr(EG(function_table), key, func_op_array);
        zend_string_release(key);
    }
    uint32_t class_count = r_u32(f);
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
    php_error_docref(NULL, E_NOTICE, "opdump: loaded %s (%u opcodes, %d literals, %u dynamic funcs, %u functions, %u classes) without parsing source",
        path, op_array->last, op_array->last_literal, op_array->num_dynamic_func_defs, function_count, class_count);
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
        if (!opdump_decrypt_bytc(blob, blob_len, &payload, &payload_len)) {
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

/* ---- module lifecycle ---- */

PHP_MINIT_FUNCTION(opdump)
{
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
    }
    return SUCCESS;
}

zend_module_entry opdump_module_entry = {
    STANDARD_MODULE_HEADER,
    "opdump",
    NULL,
    PHP_MINIT(opdump),
    NULL, NULL, NULL, NULL,
    "0.0.1-phase0",
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_OPDUMP
ZEND_GET_MODULE(opdump)
#endif
