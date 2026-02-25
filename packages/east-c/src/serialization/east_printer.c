/*
 * Printer for East text format.
 *
 * Type-driven printing: the type guides how values are printed.
 *
 * east_print_value(value, type) -> char*  (allocated string)
 * east_print_type(type) -> char*          (allocated string)
 */

#include "east/serialization.h"
#include "east/types.h"
#include "east/values.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/*  String builder                                                     */
/* ================================================================== */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} PBuf;

static PBuf pbuf_new(size_t initial_cap)
{
    PBuf sb;
    sb.cap = (initial_cap > 0) ? initial_cap : 256;
    sb.data = malloc(sb.cap);
    sb.len = 0;
    if (sb.data) sb.data[0] = '\0';
    return sb;
}

static void pbuf_ensure(PBuf *sb, size_t needed)
{
    size_t required = sb->len + needed + 1;
    if (required <= sb->cap) return;
    size_t new_cap = sb->cap * 2;
    if (new_cap < required) new_cap = required;
    char *nd = realloc(sb->data, new_cap);
    if (!nd) return;
    sb->data = nd;
    sb->cap = new_cap;
}

static void pbuf_append(PBuf *sb, const char *str, size_t len)
{
    if (len == 0) return;
    pbuf_ensure(sb, len);
    memcpy(sb->data + sb->len, str, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

static void pbuf_append_str(PBuf *sb, const char *str)
{
    pbuf_append(sb, str, strlen(str));
}

static void pbuf_append_char(PBuf *sb, char c)
{
    pbuf_ensure(sb, 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

static char *pbuf_finish(PBuf *sb)
{
    return sb->data;
}

/* ================================================================== */
/*  Identifier escaping                                                */
/* ================================================================== */

static bool needs_escaping(const char *id)
{
    if (!id || !id[0]) return true;
    if (!isalpha((unsigned char)id[0]) && id[0] != '_') return true;
    for (size_t i = 1; id[i]; i++) {
        if (!isalnum((unsigned char)id[i]) && id[i] != '_') return true;
    }
    return false;
}

static void pbuf_append_identifier(PBuf *sb, const char *id)
{
    if (needs_escaping(id)) {
        pbuf_append_char(sb, '`');
        pbuf_append_str(sb, id);
        pbuf_append_char(sb, '`');
    } else {
        pbuf_append_str(sb, id);
    }
}

/* ================================================================== */
/*  Alias tracking context                                             */
/* ================================================================== */

typedef struct {
    EastValue *ptr;          /* Container pointer identity */
    char **path;             /* Path components (owned copies) */
    size_t path_len;
    size_t path_cap;
} RefEntry;

/* Hash map for O(1) container lookup: pointer -> index in refs array */
typedef struct {
    uintptr_t key;   /* 0 = empty */
    size_t index;
} PrintRefSlot;

typedef struct {
    RefEntry *refs;
    size_t ref_count;
    size_t ref_cap;
    PrintRefSlot *map;   /* hash map slots */
    int map_mask;        /* capacity - 1 */
    char **path;             /* Current path stack (owned copies) */
    size_t path_depth;
    size_t path_cap;
} PrintContext;

static void ctx_push_path(PrintContext *ctx, const char *component) {
    if (!ctx) return;
    if (ctx->path_depth >= ctx->path_cap) {
        size_t new_cap = ctx->path_cap ? ctx->path_cap * 2 : 8;
        char **new_path = realloc(ctx->path, new_cap * sizeof(char *));
        if (!new_path) return;
        ctx->path = new_path;
        ctx->path_cap = new_cap;
    }
    ctx->path[ctx->path_depth++] = strdup(component);
}

static void ctx_pop_path(PrintContext *ctx) {
    if (ctx && ctx->path_depth > 0) {
        free(ctx->path[--ctx->path_depth]);
    }
}

static inline uint32_t print_hash_ptr(uintptr_t p) {
    p ^= p >> 16;
    p *= 0x45d9f3b;
    p ^= p >> 16;
    return (uint32_t)p;
}

static void ctx_map_grow(PrintContext *ctx) {
    int old_cap = ctx->map_mask + 1;
    int new_cap = old_cap * 2;
    int new_mask = new_cap - 1;
    PrintRefSlot *new_map = calloc((size_t)new_cap, sizeof(PrintRefSlot));
    if (!new_map) return;
    for (int i = 0; i < old_cap; i++) {
        if (ctx->map[i].key != 0) {
            uint32_t h = print_hash_ptr(ctx->map[i].key) & (uint32_t)new_mask;
            while (new_map[h].key != 0) h = (h + 1) & (uint32_t)new_mask;
            new_map[h] = ctx->map[i];
        }
    }
    free(ctx->map);
    ctx->map = new_map;
    ctx->map_mask = new_mask;
}

static void ctx_register(PrintContext *ctx, EastValue *ptr) {
    if (!ctx) return;
    if (ctx->ref_count >= ctx->ref_cap) {
        size_t new_cap = ctx->ref_cap ? ctx->ref_cap * 2 : 8;
        RefEntry *new_refs = realloc(ctx->refs, new_cap * sizeof(RefEntry));
        if (!new_refs) return;
        ctx->refs = new_refs;
        ctx->ref_cap = new_cap;
    }
    size_t idx = ctx->ref_count;
    RefEntry *e = &ctx->refs[ctx->ref_count++];
    e->ptr = ptr;
    e->path_len = ctx->path_depth;
    e->path_cap = ctx->path_depth;
    if (ctx->path_depth > 0) {
        e->path = malloc(ctx->path_depth * sizeof(char *));
        if (!e->path) { e->path_len = 0; e->path_cap = 0; return; }
        for (size_t i = 0; i < ctx->path_depth; i++) {
            e->path[i] = strdup(ctx->path[i]);
        }
    } else {
        e->path = NULL;
    }
    /* Insert into hash map */
    if (!ctx->map) {
        ctx->map_mask = 31;
        ctx->map = calloc(32, sizeof(PrintRefSlot));
    }
    if ((int)ctx->ref_count * 10 >= (ctx->map_mask + 1) * 7)
        ctx_map_grow(ctx);
    uintptr_t key = (uintptr_t)ptr;
    uint32_t h = print_hash_ptr(key) & (uint32_t)ctx->map_mask;
    while (ctx->map[h].key != 0) h = (h + 1) & (uint32_t)ctx->map_mask;
    ctx->map[h].key = key;
    ctx->map[h].index = idx;
}

/* Check if a container was already seen. Returns the entry or NULL. */
static RefEntry *ctx_find(PrintContext *ctx, EastValue *ptr) {
    if (!ctx || !ctx->map) return NULL;
    uintptr_t key = (uintptr_t)ptr;
    uint32_t h = print_hash_ptr(key) & (uint32_t)ctx->map_mask;
    for (;;) {
        if (ctx->map[h].key == key) return &ctx->refs[ctx->map[h].index];
        if (ctx->map[h].key == 0) return NULL;
        h = (h + 1) & (uint32_t)ctx->map_mask;
    }
}

/* Emit a relative backreference: upLevels#remainingPath */
static void emit_backref(PBuf *sb, PrintContext *ctx, RefEntry *target) {
    /* Find common prefix length between current path and target path */
    size_t common = 0;
    size_t cur_len = ctx->path_depth;
    size_t tgt_len = target->path_len;
    while (common < cur_len && common < tgt_len &&
           strcmp(ctx->path[common], target->path[common]) == 0) {
        common++;
    }
    int up_levels = (int)(cur_len - common);
    char numbuf[16];
    snprintf(numbuf, sizeof(numbuf), "%d#", up_levels);
    pbuf_append_str(sb, numbuf);
    /* Append remaining path components from target */
    for (size_t i = common; i < tgt_len; i++) {
        pbuf_append_str(sb, target->path[i]);
    }
}

static void ctx_free(PrintContext *ctx) {
    if (!ctx) return;
    for (size_t i = 0; i < ctx->ref_count; i++) {
        for (size_t j = 0; j < ctx->refs[i].path_len; j++) {
            free(ctx->refs[i].path[j]);
        }
        free(ctx->refs[i].path);
    }
    free(ctx->refs);
    free(ctx->map);
    for (size_t i = 0; i < ctx->path_depth; i++) {
        free(ctx->path[i]);
    }
    free(ctx->path);
}

/* ================================================================== */
/*  Value printer                                                      */
/* ================================================================== */

static void print_val(PBuf *sb, EastValue *value, EastType *type, PrintContext *ctx);

static void print_val(PBuf *sb, EastValue *value, EastType *type, PrintContext *ctx)
{
    if (!type || !value) {
        pbuf_append_str(sb, "null");
        return;
    }

    switch (type->kind) {
    case EAST_TYPE_NEVER:
    case EAST_TYPE_NULL:
        pbuf_append_str(sb, "null");
        break;

    case EAST_TYPE_BOOLEAN:
        pbuf_append_str(sb, value->data.boolean ? "true" : "false");
        break;

    case EAST_TYPE_INTEGER: {
        char numbuf[32];
        snprintf(numbuf, sizeof(numbuf), "%lld", (long long)value->data.integer);
        pbuf_append_str(sb, numbuf);
        break;
    }

    case EAST_TYPE_FLOAT: {
        double f = value->data.float64;
        if (f != f) {
            pbuf_append_str(sb, "NaN");
        } else if (isinf(f)) {
            pbuf_append_str(sb, f > 0 ? "Infinity" : "-Infinity");
        } else if (f == 0.0 && signbit(f)) {
            pbuf_append_str(sb, "-0.0");
        } else {
            char numbuf[64];
            east_fmt_double(numbuf, sizeof(numbuf), f);
            /* Ensure there's a decimal point for float distinction */
            if (!strchr(numbuf, '.') && !strchr(numbuf, 'e') && !strchr(numbuf, 'E')) {
                size_t nlen = strlen(numbuf);
                numbuf[nlen] = '.';
                numbuf[nlen+1] = '0';
                numbuf[nlen+2] = '\0';
            }
            pbuf_append_str(sb, numbuf);
        }
        break;
    }

    case EAST_TYPE_STRING: {
        /* East text format: only escape \\ and \" */
        pbuf_append_char(sb, '"');
        for (size_t i = 0; i < value->data.string.len; i++) {
            char c = value->data.string.data[i];
            if (c == '\\') {
                pbuf_append_str(sb, "\\\\");
            } else if (c == '"') {
                pbuf_append_str(sb, "\\\"");
            } else {
                pbuf_append_char(sb, c);
            }
        }
        pbuf_append_char(sb, '"');
        break;
    }

    case EAST_TYPE_DATETIME: {
        /* ISO 8601 with milliseconds, no timezone suffix
         * (matches JavaScript toISOString().substring(0,23)) */
        int64_t millis = value->data.datetime;
        int64_t secs = millis / 1000;
        int64_t ms = millis % 1000;
        if (ms < 0) { ms += 1000; secs--; }

        int64_t days = secs / 86400;
        int64_t rem = secs % 86400;
        if (rem < 0) { rem += 86400; days--; }

        int hour = (int)(rem / 3600);
        rem %= 3600;
        int min = (int)(rem / 60);
        int sec = (int)(rem % 60);

        /* Days to y/m/d */
        int64_t z = days + 719468;
        int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        int64_t doe = z - era * 146097;
        int64_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
        int64_t y = yoe + era * 400;
        int64_t doy = doe - (365*yoe + yoe/4 - yoe/100);
        int64_t mp = (5*doy + 2) / 153;
        int64_t d = doy - (153*mp + 2)/5 + 1;
        int64_t m = mp + (mp < 10 ? 3 : -9);
        y += (m <= 2) ? 1 : 0;

        char datebuf[32];
        snprintf(datebuf, sizeof(datebuf),
                 "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
                 (int)y, (int)m, (int)d, hour, min, sec, (int)ms);
        pbuf_append_str(sb, datebuf);
        break;
    }

    case EAST_TYPE_BLOB: {
        pbuf_append_str(sb, "0x");
        for (size_t i = 0; i < value->data.blob.len; i++) {
            char hex[3];
            snprintf(hex, sizeof(hex), "%02x", value->data.blob.data[i]);
            pbuf_append_str(sb, hex);
        }
        break;
    }

    case EAST_TYPE_ARRAY: {
        /* Check for alias */
        if (ctx) {
            RefEntry *existing = ctx_find(ctx, value);
            if (existing) {
                emit_backref(sb, ctx, existing);
                break;
            }
            ctx_register(ctx, value);
        }
        EastType *elem_type = type->data.element;
        size_t count = value->data.array.len;
        if (count == 0) {
            pbuf_append_str(sb, "[]");
        } else {
            pbuf_append_char(sb, '[');
            for (size_t i = 0; i < count; i++) {
                if (i > 0) pbuf_append_str(sb, ", ");
                char idx_buf[24];
                snprintf(idx_buf, sizeof(idx_buf), "[%zu]", i);
                ctx_push_path(ctx, idx_buf);
                print_val(sb, value->data.array.items[i], elem_type, ctx);
                ctx_pop_path(ctx);
            }
            pbuf_append_char(sb, ']');
        }
        break;
    }

    case EAST_TYPE_SET: {
        /* Check for alias */
        if (ctx) {
            RefEntry *existing = ctx_find(ctx, value);
            if (existing) {
                emit_backref(sb, ctx, existing);
                break;
            }
            ctx_register(ctx, value);
        }
        EastType *elem_type = type->data.element;
        size_t count = value->data.set.len;
        if (count == 0) {
            pbuf_append_str(sb, "{}");
        } else {
            pbuf_append_char(sb, '{');
            for (size_t i = 0; i < count; i++) {
                if (i > 0) pbuf_append_char(sb, ',');
                print_val(sb, value->data.set.items[i], elem_type, ctx);
            }
            pbuf_append_char(sb, '}');
        }
        break;
    }

    case EAST_TYPE_DICT: {
        /* Check for alias */
        if (ctx) {
            RefEntry *existing = ctx_find(ctx, value);
            if (existing) {
                emit_backref(sb, ctx, existing);
                break;
            }
            ctx_register(ctx, value);
        }
        EastType *key_type = type->data.dict.key;
        EastType *val_type = type->data.dict.value;
        size_t count = value->data.dict.len;
        if (count == 0) {
            pbuf_append_str(sb, "{:}");
        } else {
            pbuf_append_char(sb, '{');
            for (size_t i = 0; i < count; i++) {
                if (i > 0) pbuf_append_char(sb, ',');
                print_val(sb, value->data.dict.keys[i], key_type, ctx);
                pbuf_append_char(sb, ':');
                print_val(sb, value->data.dict.values[i], val_type, ctx);
            }
            pbuf_append_char(sb, '}');
        }
        break;
    }

    case EAST_TYPE_STRUCT: {
        size_t nf = type->data.struct_.num_fields;
        if (nf == 0) {
            pbuf_append_str(sb, "()");
        } else {
            pbuf_append_char(sb, '(');
            for (size_t i = 0; i < nf; i++) {
                if (i > 0) pbuf_append_str(sb, ", ");
                const char *fname = type->data.struct_.fields[i].name;
                EastType *ftype = type->data.struct_.fields[i].type;

                pbuf_append_identifier(sb, fname);
                pbuf_append_char(sb, '=');

                /* Struct values always have fields in type schema order */
                EastValue *fval = (value->kind == EAST_VAL_STRUCT && i < value->data.struct_.num_fields)
                                ? value->data.struct_.field_values[i] : NULL;
                /* Push field path component: ".fieldname" */
                char path_buf[256];
                snprintf(path_buf, sizeof(path_buf), ".%s", fname);
                ctx_push_path(ctx, path_buf);
                print_val(sb, fval, ftype, ctx);
                ctx_pop_path(ctx);
            }
            pbuf_append_char(sb, ')');
        }
        break;
    }

    case EAST_TYPE_VARIANT: {
        if (value->kind != EAST_VAL_VARIANT) {
            pbuf_append_str(sb, "null");
            break;
        }
        const char *case_name = value->data.variant.case_name;
        if (!case_name) {
            pbuf_append_str(sb, "null");
            break;
        }
        EastType *case_type = NULL;
        for (size_t i = 0; i < type->data.variant.num_cases; i++) {
            if (strcmp(type->data.variant.cases[i].name, case_name) == 0) {
                case_type = type->data.variant.cases[i].type;
                break;
            }
        }

        pbuf_append_char(sb, '.');
        pbuf_append_str(sb, case_name);

        /* Print value if not null */
        if (case_type && case_type->kind != EAST_TYPE_NULL &&
            value->data.variant.value &&
            value->data.variant.value->kind != EAST_VAL_NULL) {
            pbuf_append_char(sb, ' ');
            print_val(sb, value->data.variant.value, case_type, ctx);
        }
        break;
    }

    case EAST_TYPE_REF: {
        /* Check for alias */
        if (ctx) {
            RefEntry *existing = ctx_find(ctx, value);
            if (existing) {
                emit_backref(sb, ctx, existing);
                break;
            }
            ctx_register(ctx, value);
        }
        pbuf_append_char(sb, '&');
        print_val(sb, value->data.ref.value, type->data.element, ctx);
        break;
    }

    case EAST_TYPE_VECTOR: {
        EastType *elem_type = type->data.element;
        size_t vlen = value->data.vector.len;
        if (vlen == 0) {
            pbuf_append_str(sb, "vec[]");
        } else {
            pbuf_append_str(sb, "vec[");
            for (size_t i = 0; i < vlen; i++) {
                if (i > 0) pbuf_append_str(sb, ", ");
                char numbuf[64];
                if (elem_type->kind == EAST_TYPE_FLOAT) {
                    double v = ((double *)value->data.vector.data)[i];
                    if (v != v) {
                        pbuf_append_str(sb, "NaN");
                    } else if (isinf(v)) {
                        pbuf_append_str(sb, v > 0 ? "Infinity" : "-Infinity");
                    } else {
                        east_fmt_double(numbuf, sizeof(numbuf), v);
                        if (!strchr(numbuf, '.') && !strchr(numbuf, 'e')) {
                            size_t nlen = strlen(numbuf);
                            numbuf[nlen] = '.'; numbuf[nlen+1] = '0'; numbuf[nlen+2] = '\0';
                        }
                        pbuf_append_str(sb, numbuf);
                    }
                } else if (elem_type->kind == EAST_TYPE_INTEGER) {
                    snprintf(numbuf, sizeof(numbuf), "%lld",
                             (long long)((int64_t *)value->data.vector.data)[i]);
                    pbuf_append_str(sb, numbuf);
                } else if (elem_type->kind == EAST_TYPE_BOOLEAN) {
                    bool bv = ((bool *)value->data.vector.data)[i];
                    pbuf_append_str(sb, bv ? "true" : "false");
                }
            }
            pbuf_append_char(sb, ']');
        }
        break;
    }

    case EAST_TYPE_MATRIX: {
        EastType *elem_type = type->data.element;
        size_t rows = value->data.matrix.rows;
        size_t cols = value->data.matrix.cols;

        if (rows == 0 || cols == 0) {
            pbuf_append_str(sb, "mat[]");
        } else {
            pbuf_append_str(sb, "mat[");
            for (size_t r = 0; r < rows; r++) {
                if (r > 0) pbuf_append_str(sb, ", ");
                pbuf_append_char(sb, '[');
                for (size_t c = 0; c < cols; c++) {
                    if (c > 0) pbuf_append_str(sb, ", ");
                    size_t idx = r * cols + c;
                    char numbuf[64];
                    if (elem_type->kind == EAST_TYPE_FLOAT) {
                        double v = ((double *)value->data.matrix.data)[idx];
                        if (v != v) {
                            pbuf_append_str(sb, "NaN");
                        } else if (isinf(v)) {
                            pbuf_append_str(sb, v > 0 ? "Infinity" : "-Infinity");
                        } else {
                            east_fmt_double(numbuf, sizeof(numbuf), v);
                            if (!strchr(numbuf, '.') && !strchr(numbuf, 'e')) {
                                size_t nlen = strlen(numbuf);
                                numbuf[nlen] = '.'; numbuf[nlen+1] = '0'; numbuf[nlen+2] = '\0';
                            }
                            pbuf_append_str(sb, numbuf);
                        }
                    } else if (elem_type->kind == EAST_TYPE_INTEGER) {
                        snprintf(numbuf, sizeof(numbuf), "%lld",
                                 (long long)((int64_t *)value->data.matrix.data)[idx]);
                        pbuf_append_str(sb, numbuf);
                    } else if (elem_type->kind == EAST_TYPE_BOOLEAN) {
                        bool bv = ((bool *)value->data.matrix.data)[idx];
                        pbuf_append_str(sb, bv ? "true" : "false");
                    }
                }
                pbuf_append_char(sb, ']');
            }
            pbuf_append_char(sb, ']');
        }
        break;
    }

    case EAST_TYPE_RECURSIVE:
        /* Unwrap: print via the inner node type */
        if (type->data.recursive.node) {
            print_val(sb, value, type->data.recursive.node, ctx);
        } else {
            pbuf_append_str(sb, "null");
        }
        break;

    case EAST_TYPE_FUNCTION:
    case EAST_TYPE_ASYNC_FUNCTION:
        /* Lambda symbol (UTF-8 for U+03BB) */
        pbuf_append_str(sb, "\xCE\xBB");
        break;
    }
}

char *east_print_value(EastValue *value, EastType *type)
{
    PBuf sb = pbuf_new(256);
    PrintContext ctx = {0};
    print_val(&sb, value, type, &ctx);
    ctx_free(&ctx);
    return pbuf_finish(&sb);
}

/* ================================================================== */
/*  Type printer                                                       */
/* ================================================================== */

static void print_type_internal(PBuf *sb, EastType *type);

static void print_type_internal(PBuf *sb, EastType *type)
{
    if (!type) {
        pbuf_append_str(sb, "(null)");
        return;
    }

    switch (type->kind) {
    case EAST_TYPE_NEVER:    pbuf_append_str(sb, ".Never"); break;
    case EAST_TYPE_NULL:     pbuf_append_str(sb, ".Null"); break;
    case EAST_TYPE_BOOLEAN:  pbuf_append_str(sb, ".Boolean"); break;
    case EAST_TYPE_INTEGER:  pbuf_append_str(sb, ".Integer"); break;
    case EAST_TYPE_FLOAT:    pbuf_append_str(sb, ".Float"); break;
    case EAST_TYPE_STRING:   pbuf_append_str(sb, ".String"); break;
    case EAST_TYPE_DATETIME: pbuf_append_str(sb, ".DateTime"); break;
    case EAST_TYPE_BLOB:     pbuf_append_str(sb, ".Blob"); break;

    case EAST_TYPE_ARRAY:
        pbuf_append_str(sb, ".Array ");
        print_type_internal(sb, type->data.element);
        break;

    case EAST_TYPE_SET:
        pbuf_append_str(sb, ".Set ");
        print_type_internal(sb, type->data.element);
        break;

    case EAST_TYPE_VECTOR:
        pbuf_append_str(sb, ".Vector ");
        print_type_internal(sb, type->data.element);
        break;

    case EAST_TYPE_MATRIX:
        pbuf_append_str(sb, ".Matrix ");
        print_type_internal(sb, type->data.element);
        break;

    case EAST_TYPE_REF:
        pbuf_append_str(sb, ".Ref ");
        print_type_internal(sb, type->data.element);
        break;

    case EAST_TYPE_DICT:
        pbuf_append_str(sb, ".Dict (key=");
        print_type_internal(sb, type->data.dict.key);
        pbuf_append_str(sb, ", value=");
        print_type_internal(sb, type->data.dict.value);
        pbuf_append_char(sb, ')');
        break;

    case EAST_TYPE_STRUCT: {
        pbuf_append_str(sb, ".Struct [");
        for (size_t i = 0; i < type->data.struct_.num_fields; i++) {
            if (i > 0) pbuf_append_str(sb, ", ");
            pbuf_append_str(sb, "(name=");
            /* Print field name as JSON string */
            pbuf_append_char(sb, '"');
            const char *name = type->data.struct_.fields[i].name;
            for (size_t j = 0; name[j]; j++) {
                if (name[j] == '"') pbuf_append_str(sb, "\\\"");
                else if (name[j] == '\\') pbuf_append_str(sb, "\\\\");
                else pbuf_append_char(sb, name[j]);
            }
            pbuf_append_char(sb, '"');
            pbuf_append_str(sb, ", type=");
            print_type_internal(sb, type->data.struct_.fields[i].type);
            pbuf_append_char(sb, ')');
        }
        pbuf_append_char(sb, ']');
        break;
    }

    case EAST_TYPE_VARIANT: {
        pbuf_append_str(sb, ".Variant [");
        for (size_t i = 0; i < type->data.variant.num_cases; i++) {
            if (i > 0) pbuf_append_str(sb, ", ");
            pbuf_append_str(sb, "(name=");
            pbuf_append_char(sb, '"');
            const char *name = type->data.variant.cases[i].name;
            for (size_t j = 0; name[j]; j++) {
                if (name[j] == '"') pbuf_append_str(sb, "\\\"");
                else if (name[j] == '\\') pbuf_append_str(sb, "\\\\");
                else pbuf_append_char(sb, name[j]);
            }
            pbuf_append_char(sb, '"');
            pbuf_append_str(sb, ", type=");
            print_type_internal(sb, type->data.variant.cases[i].type);
            pbuf_append_char(sb, ')');
        }
        pbuf_append_char(sb, ']');
        break;
    }

    case EAST_TYPE_FUNCTION:
    case EAST_TYPE_ASYNC_FUNCTION: {
        const char *prefix = (type->kind == EAST_TYPE_ASYNC_FUNCTION)
                             ? ".AsyncFunction" : ".Function";
        pbuf_append_str(sb, prefix);
        pbuf_append_str(sb, " (inputs=[");
        for (size_t i = 0; i < type->data.function.num_inputs; i++) {
            if (i > 0) pbuf_append_str(sb, ", ");
            print_type_internal(sb, type->data.function.inputs[i]);
        }
        pbuf_append_str(sb, "], output=");
        print_type_internal(sb, type->data.function.output);
        pbuf_append_char(sb, ')');
        break;
    }

    case EAST_TYPE_RECURSIVE: {
        pbuf_append_str(sb, ".Recursive (...)");
        break;
    }
    }
}

char *east_print_type(EastType *type)
{
    PBuf sb = pbuf_new(256);
    print_type_internal(&sb, type);
    return pbuf_finish(&sb);
}
