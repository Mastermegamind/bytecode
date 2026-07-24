/*
 * opdump - Phase 0 feasibility spike.
 *
 * Hooks zend_compile_file so that, in "dump" mode, a normally-compiled
 * top-level zend_op_array is serialized to disk after compilation, and in
 * "load" mode a previously-dumped op_array is reconstructed from disk
 * *without* the engine ever parsing PHP source text for that request.
 *
 * Scope (see docs/PHASE0.md in the repo root): main/top-level op_arrays
 * only, literal types NULL/BOOL/LONG/DOUBLE/STRING only, no functions,
 * classes, CV variables, try/catch, or static variables yet. Each of those
 * is a deliberate next rung, not an oversight.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "zend_compile.h"
#include "zend_vm.h"
#include "zend_string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static zend_op_array *(*opdump_orig_compile_file)(zend_file_handle *file_handle, int type);

/* ---- binary format helpers ---- */

#define OPDUMP_MAGIC "OPD1"

typedef enum {
    OPDUMP_LIT_NULL  = 0,
    OPDUMP_LIT_TRUE  = 1,
    OPDUMP_LIT_FALSE = 2,
    OPDUMP_LIT_LONG  = 3,
    OPDUMP_LIT_DOUBLE = 4,
    OPDUMP_LIT_STRING = 5
} opdump_lit_tag;

static void w_u8(FILE *f, uint8_t v)   { fwrite(&v, 1, 1, f); }
static void w_u32(FILE *f, uint32_t v) { fwrite(&v, sizeof(v), 1, f); }
static void w_i32(FILE *f, int32_t v)  { fwrite(&v, sizeof(v), 1, f); }
static void w_dbl(FILE *f, double v)   { fwrite(&v, sizeof(v), 1, f); }
static void w_str(FILE *f, const char *s, size_t len) {
    w_u32(f, (uint32_t)len);
    if (len) fwrite(s, 1, len, f);
}

static uint8_t  r_u8(FILE *f)  { uint8_t v = 0; size_t n = fread(&v, 1, 1, f); (void)n; return v; }
static uint32_t r_u32(FILE *f) { uint32_t v = 0; size_t n = fread(&v, sizeof(v), 1, f); (void)n; return v; }
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

/* ---- dump ---- */

static void opdump_write(zend_op_array *op_array, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        php_error_docref(NULL, E_WARNING, "opdump: cannot open %s for writing", path);
        return;
    }
    fwrite(OPDUMP_MAGIC, 1, 4, f);
    w_u32(f, PHP_VERSION_ID);

    w_u8(f, op_array->type);
    w_u32(f, op_array->fn_flags);
    w_u32(f, op_array->T);
    w_i32(f, op_array->cache_size);

    w_str(f, op_array->filename ? ZSTR_VAL(op_array->filename) : "", op_array->filename ? ZSTR_LEN(op_array->filename) : 0);
    w_u32(f, op_array->line_start);
    w_u32(f, op_array->line_end);

    /* opcode count and literal count both come before the opcode records
     * themselves (rather than the more "natural" opcodes-then-literals
     * order) so the loader can size+allocate the combined opcodes+literals
     * block up front, matching pass_two()'s layout, without needing to
     * seek ahead in the stream. */
    w_u32(f, op_array->last);
    w_u32(f, (uint32_t)op_array->last_literal);
    for (uint32_t i = 0; i < op_array->last; i++) {
        zend_op *op = &op_array->opcodes[i];
        w_u8(f, op->opcode);
        w_u8(f, op->op1_type);
        w_u8(f, op->op2_type);
        w_u8(f, op->result_type);
        w_u32(f, op->extended_value);
        w_u32(f, op->lineno);
        /* znode_op is a plain uint32 union on 64-bit builds (no abs
         * pointer members compiled in), so this is a safe, portable,
         * position-independent copy. */
        w_u32(f, op->op1.num);
        w_u32(f, op->op2.num);
        w_u32(f, op->result.num);
    }

    /* literals */
    for (int i = 0; i < op_array->last_literal; i++) {
        zval *zv = &op_array->literals[i];
        switch (Z_TYPE_P(zv)) {
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

    fclose(f);
    php_error_docref(NULL, E_NOTICE, "opdump: wrote %s (%u opcodes, %d literals)",
        path, op_array->last, op_array->last_literal);
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

static zend_op_array *opdump_read(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        php_error_docref(NULL, E_ERROR, "opdump: cannot open %s for reading", path);
        return NULL;
    }

    char magic[4];
    size_t n = fread(magic, 1, 4, f); (void)n;
    if (memcmp(magic, OPDUMP_MAGIC, 4) != 0) {
        php_error_docref(NULL, E_ERROR, "opdump: bad magic in %s", path);
        fclose(f);
        return NULL;
    }
    uint32_t built_for_version = r_u32(f);
    if (built_for_version != PHP_VERSION_ID) {
        php_error_docref(NULL, E_WARNING,
            "opdump: blob was dumped under PHP_VERSION_ID %u, running %u -- proceeding anyway for this spike",
            built_for_version, PHP_VERSION_ID);
    }

    uint8_t saved_type = r_u8(f);
    uint32_t saved_fn_flags = r_u32(f);
    uint32_t saved_T = r_u32(f);
    int32_t saved_cache_size = r_i32(f);

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
    op_array->fn_flags = saved_fn_flags;
    op_array->T        = saved_T;
    op_array->cache_size = saved_cache_size;
    op_array->line_start = r_u32(f);
    op_array->line_end   = r_u32(f);

    uint32_t last = r_u32(f);
    int last_literal = (int) r_u32(f);

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
        zend_vm_set_opcode_handler(op);
    }

    op_array->last_literal = last_literal;
    for (int i = 0; i < last_literal; i++) {
        zval *zv = &op_array->literals[i];
        uint8_t tag = r_u8(f);
        switch (tag) {
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

    fclose(f);
    php_error_docref(NULL, E_NOTICE, "opdump: loaded %s (%u opcodes, %d literals) without parsing source",
        path, op_array->last, op_array->last_literal);

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

/* ---- module lifecycle ---- */

PHP_MINIT_FUNCTION(opdump)
{
    opdump_orig_compile_file = zend_compile_file;
    const char *mode = getenv("OPDUMP_MODE");
    if (mode && strcmp(mode, "dump") == 0) {
        zend_compile_file = opdump_dump_compile_file;
    } else if (mode && strcmp(mode, "load") == 0) {
        zend_compile_file = opdump_load_compile_file;
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
