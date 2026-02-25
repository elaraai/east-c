/*
 * BEAST2 binary serialization for East types.
 *
 * Headerless binary format with varint encoding.
 * Type-driven: no type tags in the output; the type guides encoding/decoding.
 *
 * Encoding format:
 *   Null:     nothing (0 bytes)
 *   Boolean:  1 byte (0 or 1)
 *   Integer:  zigzag-encoded varint
 *   Float:    8 bytes little-endian IEEE 754
 *   String:   varint length + UTF-8 bytes
 *   DateTime: zigzag varint (epoch millis)
 *   Blob:     varint length + raw bytes
 *   Array:    varint count + each element
 *   Set:      varint count + each element
 *   Dict:     varint count + each key-value pair
 *   Struct:   each field in schema order
 *   Variant:  varint case index + case value
 *   Ref:      encode inner value
 *   Vector:   varint length + packed elements
 *   Matrix:   varint rows + varint cols + packed elements
 */

#include "east/serialization.h"
#include "east/types.h"
#include "east/values.h"
#include "east/compiler.h"
#include "east/type_of_type.h"
#include "east/env.h"
#include "east/ir.h"

#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/*  Helpers for little-endian float writing/reading                    */
/* ================================================================== */

static void write_float64_le(ByteBuffer *buf, double val)
{
    uint8_t bytes[8];
    memcpy(bytes, &val, 8);
    /* On big-endian systems this would need byte-swapping.
     * Assuming little-endian (x86, ARM LE) for simplicity. */
    byte_buffer_write_bytes(buf, bytes, 8);
}

static double read_float64_le(const uint8_t *data, size_t *offset)
{
    double val;
    memcpy(&val, data + *offset, 8);
    *offset += 8;
    return val;
}

/* Read a varint-prefixed string, returning malloc'd string and setting *out_len */
static char *read_string_varint(const uint8_t *data, size_t len, size_t *offset, size_t *out_len)
{
    uint64_t slen = read_varint(data, offset);
    if (*offset + slen > len) { *out_len = 0; return NULL; }
    char *str = malloc(slen + 1);
    if (!str) { *out_len = 0; return NULL; }
    memcpy(str, data + *offset, slen);
    str[slen] = '\0';
    *offset += slen;
    *out_len = (size_t)slen;
    return str;
}

/* ================================================================== */
/*  BEAST2 Backreference Context                                       */
/*                                                                     */
/*  Mutable containers (Array, Set, Dict, Ref) use a backreference     */
/*  protocol: varint(0) = inline (first occurrence), varint(N>0) =     */
/*  backreference (N = distance in bytes from current pos to stored).  */
/* ================================================================== */

/*
 * Open-addressing hash tables for O(1) backreference lookup.
 * Encode ctx: key = EastValue* pointer -> value = byte offset
 * Decode ctx: key = byte offset -> value = EastValue*
 */

typedef struct {
    uintptr_t key;     /* 0 = empty slot */
    size_t offset;
} Beast2EncSlot;

typedef struct {
    Beast2EncSlot *slots;
    int mask;          /* capacity - 1 (capacity is power of 2) */
    int count;
} Beast2EncodeCtx;

typedef struct {
    size_t key;        /* 0 = empty (offset 0 never used as backreference target) */
    EastValue *value;
} Beast2DecSlot;

typedef struct {
    Beast2DecSlot *slots;
    int mask;
    int count;
} Beast2DecodeCtx;

static inline uint32_t hash_ptr(uintptr_t p)
{
    /* Fibonacci hashing — good distribution for pointer values */
    p ^= p >> 16;
    p *= 0x45d9f3b;
    p ^= p >> 16;
    return (uint32_t)p;
}

static inline uint32_t hash_offset(size_t o)
{
    uintptr_t p = (uintptr_t)o;
    p ^= p >> 16;
    p *= 0x45d9f3b;
    p ^= p >> 16;
    return (uint32_t)p;
}

static void beast2_enc_ctx_init(Beast2EncodeCtx *ctx)
{
    ctx->mask = 63;  /* initial capacity 64 */
    ctx->count = 0;
    ctx->slots = calloc((size_t)(ctx->mask + 1), sizeof(Beast2EncSlot));
}

static void beast2_enc_ctx_free(Beast2EncodeCtx *ctx)
{
    free(ctx->slots);
}

static void beast2_enc_ctx_grow(Beast2EncodeCtx *ctx)
{
    int old_cap = ctx->mask + 1;
    int new_cap = old_cap * 2;
    int new_mask = new_cap - 1;
    Beast2EncSlot *new_slots = calloc((size_t)new_cap, sizeof(Beast2EncSlot));
    if (!new_slots) return;

    for (int i = 0; i < old_cap; i++) {
        if (ctx->slots[i].key != 0) {
            uint32_t h = hash_ptr(ctx->slots[i].key) & (uint32_t)new_mask;
            while (new_slots[h].key != 0)
                h = (h + 1) & (uint32_t)new_mask;
            new_slots[h] = ctx->slots[i];
        }
    }
    free(ctx->slots);
    ctx->slots = new_slots;
    ctx->mask = new_mask;
}

/* Look up a value in the encode context. Returns -1 if not found, else the stored offset. */
static int beast2_enc_ctx_find(Beast2EncodeCtx *ctx, EastValue *value)
{
    uintptr_t key = (uintptr_t)value;
    uint32_t h = hash_ptr(key) & (uint32_t)ctx->mask;
    for (;;) {
        if (ctx->slots[h].key == key)
            return (int)ctx->slots[h].offset;
        if (ctx->slots[h].key == 0)
            return -1;
        h = (h + 1) & (uint32_t)ctx->mask;
    }
}

static void beast2_enc_ctx_add(Beast2EncodeCtx *ctx, EastValue *value, size_t offset)
{
    /* Grow at 70% load */
    if (ctx->count * 10 >= (ctx->mask + 1) * 7)
        beast2_enc_ctx_grow(ctx);

    uintptr_t key = (uintptr_t)value;
    uint32_t h = hash_ptr(key) & (uint32_t)ctx->mask;
    while (ctx->slots[h].key != 0)
        h = (h + 1) & (uint32_t)ctx->mask;
    ctx->slots[h].key = key;
    ctx->slots[h].offset = offset;
    ctx->count++;
}

static void beast2_dec_ctx_init(Beast2DecodeCtx *ctx)
{
    ctx->mask = 63;
    ctx->count = 0;
    ctx->slots = calloc((size_t)(ctx->mask + 1), sizeof(Beast2DecSlot));
}

static void beast2_dec_ctx_free(Beast2DecodeCtx *ctx)
{
    free(ctx->slots);
}

static void beast2_dec_ctx_grow(Beast2DecodeCtx *ctx)
{
    int old_cap = ctx->mask + 1;
    int new_cap = old_cap * 2;
    int new_mask = new_cap - 1;
    Beast2DecSlot *new_slots = calloc((size_t)new_cap, sizeof(Beast2DecSlot));
    if (!new_slots) return;

    for (int i = 0; i < old_cap; i++) {
        if (ctx->slots[i].key != 0) {
            uint32_t h = hash_offset(ctx->slots[i].key) & (uint32_t)new_mask;
            while (new_slots[h].key != 0)
                h = (h + 1) & (uint32_t)new_mask;
            new_slots[h] = ctx->slots[i];
        }
    }
    free(ctx->slots);
    ctx->slots = new_slots;
    ctx->mask = new_mask;
}

/* Look up by offset in the decode context. Returns NULL if not found. */
static EastValue *beast2_dec_ctx_find(Beast2DecodeCtx *ctx, size_t offset)
{
    if (offset == 0) return NULL;
    uint32_t h = hash_offset(offset) & (uint32_t)ctx->mask;
    for (;;) {
        if (ctx->slots[h].key == offset)
            return ctx->slots[h].value;
        if (ctx->slots[h].key == 0)
            return NULL;
        h = (h + 1) & (uint32_t)ctx->mask;
    }
}

static void beast2_dec_ctx_add(Beast2DecodeCtx *ctx, EastValue *value, size_t offset)
{
    if (offset == 0) return;  /* offset 0 is reserved as empty sentinel */
    if (ctx->count * 10 >= (ctx->mask + 1) * 7)
        beast2_dec_ctx_grow(ctx);

    uint32_t h = hash_offset(offset) & (uint32_t)ctx->mask;
    while (ctx->slots[h].key != 0)
        h = (h + 1) & (uint32_t)ctx->mask;
    ctx->slots[h].key = offset;
    ctx->slots[h].value = value;
    ctx->count++;
}

/* ================================================================== */
/*  BEAST2 Encoder                                                     */
/* ================================================================== */

static void beast2_encode_value(ByteBuffer *buf, EastValue *value,
                                EastType *type, Beast2EncodeCtx *ctx);

static void beast2_encode_value(ByteBuffer *buf, EastValue *value,
                                EastType *type, Beast2EncodeCtx *ctx)
{
    if (!type) return;

    switch (type->kind) {
    case EAST_TYPE_NEVER:
        break;

    case EAST_TYPE_NULL:
        break;

    case EAST_TYPE_BOOLEAN:
        byte_buffer_write_u8(buf, value->data.boolean ? 1 : 0);
        break;

    case EAST_TYPE_INTEGER:
        write_zigzag(buf, value->data.integer);
        break;

    case EAST_TYPE_FLOAT:
        write_float64_le(buf, value->data.float64);
        break;

    case EAST_TYPE_STRING: {
        size_t slen = value->data.string.len;
        write_varint(buf, (uint64_t)slen);
        byte_buffer_write_bytes(buf, (const uint8_t *)value->data.string.data, slen);
        break;
    }

    case EAST_TYPE_DATETIME:
        write_zigzag(buf, value->data.datetime);
        break;

    case EAST_TYPE_BLOB: {
        size_t blen = value->data.blob.len;
        write_varint(buf, (uint64_t)blen);
        if (blen > 0)
            byte_buffer_write_bytes(buf, value->data.blob.data, blen);
        break;
    }

    case EAST_TYPE_ARRAY: {
        /* Backreference protocol */
        int ref_offset = beast2_enc_ctx_find(ctx, value);
        if (ref_offset >= 0) {
            /* Backreference: distance from current position to stored offset */
            write_varint(buf, (uint64_t)(buf->len - (size_t)ref_offset));
            break;
        }
        /* Inline: write 0, register, then encode contents */
        write_varint(buf, 0);
        beast2_enc_ctx_add(ctx, value, buf->len);

        EastType *elem_type = type->data.element;
        size_t count = value->data.array.len;
        write_varint(buf, (uint64_t)count);
        for (size_t i = 0; i < count; i++) {
            beast2_encode_value(buf, value->data.array.items[i], elem_type, ctx);
        }
        break;
    }

    case EAST_TYPE_SET: {
        int ref_offset = beast2_enc_ctx_find(ctx, value);
        if (ref_offset >= 0) {
            write_varint(buf, (uint64_t)(buf->len - (size_t)ref_offset));
            break;
        }
        write_varint(buf, 0);
        beast2_enc_ctx_add(ctx, value, buf->len);

        EastType *elem_type = type->data.element;
        size_t count = value->data.set.len;
        write_varint(buf, (uint64_t)count);
        for (size_t i = 0; i < count; i++) {
            beast2_encode_value(buf, value->data.set.items[i], elem_type, ctx);
        }
        break;
    }

    case EAST_TYPE_DICT: {
        int ref_offset = beast2_enc_ctx_find(ctx, value);
        if (ref_offset >= 0) {
            write_varint(buf, (uint64_t)(buf->len - (size_t)ref_offset));
            break;
        }
        write_varint(buf, 0);
        beast2_enc_ctx_add(ctx, value, buf->len);

        EastType *key_type = type->data.dict.key;
        EastType *val_type = type->data.dict.value;
        size_t count = value->data.dict.len;
        write_varint(buf, (uint64_t)count);
        for (size_t i = 0; i < count; i++) {
            beast2_encode_value(buf, value->data.dict.keys[i], key_type, ctx);
            beast2_encode_value(buf, value->data.dict.values[i], val_type, ctx);
        }
        break;
    }

    case EAST_TYPE_STRUCT: {
        size_t nf = type->data.struct_.num_fields;
        /* Struct values always have fields in type schema order */
        for (size_t i = 0; i < nf; i++) {
            EastType *ftype = type->data.struct_.fields[i].type;
            EastValue *fval = (value->kind == EAST_VAL_STRUCT && i < value->data.struct_.num_fields)
                            ? value->data.struct_.field_values[i] : NULL;
            if (fval) {
                beast2_encode_value(buf, fval, ftype, ctx);
            } else {
                EastValue *null_val = east_null();
                beast2_encode_value(buf, null_val, ftype, ctx);
                east_value_release(null_val);
            }
        }
        break;
    }

    case EAST_TYPE_VARIANT: {
        const char *case_name = value->data.variant.case_name;
        for (size_t i = 0; i < type->data.variant.num_cases; i++) {
            if (strcmp(type->data.variant.cases[i].name, case_name) == 0) {
                write_varint(buf, (uint64_t)i);
                beast2_encode_value(buf, value->data.variant.value,
                                    type->data.variant.cases[i].type, ctx);
                break;
            }
        }
        break;
    }

    case EAST_TYPE_REF: {
        /* Ref also uses backreference protocol */
        int ref_offset = beast2_enc_ctx_find(ctx, value);
        if (ref_offset >= 0) {
            write_varint(buf, (uint64_t)(buf->len - (size_t)ref_offset));
            break;
        }
        write_varint(buf, 0);
        beast2_enc_ctx_add(ctx, value, buf->len);

        beast2_encode_value(buf, value->data.ref.value, type->data.element, ctx);
        break;
    }

    case EAST_TYPE_VECTOR: {
        EastType *elem_type = type->data.element;
        size_t vlen = value->data.vector.len;
        write_varint(buf, (uint64_t)vlen);

        if (elem_type->kind == EAST_TYPE_FLOAT) {
            byte_buffer_write_bytes(buf,
                (const uint8_t *)value->data.vector.data,
                vlen * sizeof(double));
        } else if (elem_type->kind == EAST_TYPE_INTEGER) {
            byte_buffer_write_bytes(buf,
                (const uint8_t *)value->data.vector.data,
                vlen * sizeof(int64_t));
        } else if (elem_type->kind == EAST_TYPE_BOOLEAN) {
            byte_buffer_write_bytes(buf,
                (const uint8_t *)value->data.vector.data,
                vlen * sizeof(bool));
        }
        break;
    }

    case EAST_TYPE_MATRIX: {
        EastType *elem_type = type->data.element;
        size_t rows = value->data.matrix.rows;
        size_t cols = value->data.matrix.cols;
        write_varint(buf, (uint64_t)rows);
        write_varint(buf, (uint64_t)cols);

        size_t count = rows * cols;
        if (elem_type->kind == EAST_TYPE_FLOAT) {
            byte_buffer_write_bytes(buf,
                (const uint8_t *)value->data.matrix.data,
                count * sizeof(double));
        } else if (elem_type->kind == EAST_TYPE_INTEGER) {
            byte_buffer_write_bytes(buf,
                (const uint8_t *)value->data.matrix.data,
                count * sizeof(int64_t));
        } else if (elem_type->kind == EAST_TYPE_BOOLEAN) {
            byte_buffer_write_bytes(buf,
                (const uint8_t *)value->data.matrix.data,
                count * sizeof(bool));
        }
        break;
    }

    case EAST_TYPE_RECURSIVE:
        if (type->data.recursive.node) {
            beast2_encode_value(buf, value, type->data.recursive.node, ctx);
        }
        break;

    case EAST_TYPE_FUNCTION:
    case EAST_TYPE_ASYNC_FUNCTION: {
        EastCompiledFn *fn = value->data.function.compiled;
        if (!fn || !fn->source_ir) break;

        /* Ensure IR type is initialized */
        if (!east_ir_type) east_type_of_type_init();

        /* 1. Encode the source IR variant tree */
        beast2_encode_value(buf, fn->source_ir, east_ir_type, ctx);

        /* 2. Extract captures array from source_ir */
        EastValue *fn_struct = fn->source_ir->data.variant.value;
        EastValue *caps_arr = east_struct_get_field(fn_struct, "captures");
        size_t ncaps = (caps_arr && caps_arr->kind == EAST_VAL_ARRAY) ? caps_arr->data.array.len : 0;

        /* 3. Write capture count */
        write_varint(buf, (uint64_t)ncaps);

        /* 4. For each capture, encode its value from the environment */
        for (size_t i = 0; i < ncaps; i++) {
            EastValue *cap_var = caps_arr->data.array.items[i];
            EastValue *cap_s = cap_var->data.variant.value;
            EastValue *name_v = east_struct_get_field(cap_s, "name");
            EastValue *type_v = east_struct_get_field(cap_s, "type");
            bool is_mutable = false;
            EastValue *mut_v = east_struct_get_field(cap_s, "mutable");
            if (mut_v && mut_v->kind == EAST_VAL_BOOLEAN) is_mutable = mut_v->data.boolean;

            const char *cap_name = name_v->data.string.data;
            EastType *cap_type = east_type_from_value(type_v);

            EastValue *cap_val = env_get(fn->captures, cap_name);
            if (cap_val && is_mutable && cap_val->kind == EAST_VAL_REF) {
                EastValue *inner = east_ref_get(cap_val);
                beast2_encode_value(buf, inner, cap_type, ctx);
                east_value_release(inner);
            } else if (cap_val) {
                beast2_encode_value(buf, cap_val, cap_type, ctx);
            }

            if (cap_type) east_type_release(cap_type);
        }
        break;
    }
    }
}

ByteBuffer *east_beast2_encode(EastValue *value, EastType *type)
{
    ByteBuffer *buf = byte_buffer_new(256);
    if (!buf) return NULL;
    Beast2EncodeCtx ctx;
    beast2_enc_ctx_init(&ctx);
    beast2_encode_value(buf, value, type, &ctx);
    beast2_enc_ctx_free(&ctx);
    return buf;
}

/* ================================================================== */
/*  BEAST2 Decoder                                                     */
/* ================================================================== */

static EastValue *beast2_decode_value(const uint8_t *data, size_t len,
                                      size_t *offset, EastType *type,
                                      Beast2DecodeCtx *ctx);

static EastValue *beast2_decode_value(const uint8_t *data, size_t len,
                                      size_t *offset, EastType *type,
                                      Beast2DecodeCtx *ctx)
{
    if (!type) return NULL;

    switch (type->kind) {
    case EAST_TYPE_NEVER:
        return NULL;

    case EAST_TYPE_NULL:
        return east_null();

    case EAST_TYPE_BOOLEAN: {
        if (*offset >= len) return NULL;
        bool val = data[(*offset)++] != 0;
        return east_boolean(val);
    }

    case EAST_TYPE_INTEGER: {
        int64_t val = read_zigzag(data, offset);
        return east_integer(val);
    }

    case EAST_TYPE_FLOAT: {
        if (*offset + 8 > len) return NULL;
        double val = read_float64_le(data, offset);
        return east_float(val);
    }

    case EAST_TYPE_STRING: {
        size_t slen;
        char *str = read_string_varint(data, len, offset, &slen);
        if (!str) return NULL;
        EastValue *val = east_string_len(str, slen);
        free(str);
        return val;
    }

    case EAST_TYPE_DATETIME: {
        int64_t millis = read_zigzag(data, offset);
        return east_datetime(millis);
    }

    case EAST_TYPE_BLOB: {
        uint64_t blen = read_varint(data, offset);
        if (*offset + blen > len) return NULL;
        EastValue *val = east_blob(data + *offset, (size_t)blen);
        *offset += (size_t)blen;
        return val;
    }

    case EAST_TYPE_ARRAY: {
        /* Backreference protocol */
        size_t pre_offset = *offset;
        uint64_t distance = read_varint(data, offset);
        if (distance > 0) {
            /* Backreference: look up value at (pre_offset - distance).
             * Use pre_offset (before reading varint) to match encoder which
             * computes distance from buf->len before writing the varint. */
            size_t ref_off = pre_offset - distance;
            EastValue *ref = beast2_dec_ctx_find(ctx, ref_off);
            if (ref) {
                east_value_retain(ref);
                return ref;
            }
            return NULL;
        }
        /* Inline: store offset, decode contents */
        size_t content_off = *offset;

        EastType *elem_type = type->data.element;
        uint64_t count = read_varint(data, offset);
        EastValue *arr = east_array_new(elem_type);
        if (!arr) return NULL;

        beast2_dec_ctx_add(ctx, arr, content_off);

        for (uint64_t i = 0; i < count; i++) {
            EastValue *elem = beast2_decode_value(data, len, offset, elem_type, ctx);
            if (!elem) { east_value_release(arr); return NULL; }
            east_array_push(arr, elem);
            east_value_release(elem);
        }
        return arr;
    }

    case EAST_TYPE_SET: {
        size_t pre_offset = *offset;
        uint64_t distance = read_varint(data, offset);
        if (distance > 0) {
            size_t ref_off = pre_offset - distance;
            EastValue *ref = beast2_dec_ctx_find(ctx, ref_off);
            if (ref) { east_value_retain(ref); return ref; }
            return NULL;
        }
        size_t content_off = *offset;

        EastType *elem_type = type->data.element;
        uint64_t count = read_varint(data, offset);
        EastValue *set = east_set_new(elem_type);
        if (!set) return NULL;

        beast2_dec_ctx_add(ctx, set, content_off);

        for (uint64_t i = 0; i < count; i++) {
            EastValue *elem = beast2_decode_value(data, len, offset, elem_type, ctx);
            if (!elem) { east_value_release(set); return NULL; }
            east_set_insert(set, elem);
            east_value_release(elem);
        }
        return set;
    }

    case EAST_TYPE_DICT: {
        size_t pre_offset = *offset;
        uint64_t distance = read_varint(data, offset);
        if (distance > 0) {
            size_t ref_off = pre_offset - distance;
            EastValue *ref = beast2_dec_ctx_find(ctx, ref_off);
            if (ref) { east_value_retain(ref); return ref; }
            return NULL;
        }
        size_t content_off = *offset;

        EastType *key_type = type->data.dict.key;
        EastType *val_type = type->data.dict.value;
        uint64_t count = read_varint(data, offset);
        EastValue *dict = east_dict_new(key_type, val_type);
        if (!dict) return NULL;

        beast2_dec_ctx_add(ctx, dict, content_off);

        for (uint64_t i = 0; i < count; i++) {
            EastValue *k = beast2_decode_value(data, len, offset, key_type, ctx);
            if (!k) { east_value_release(dict); return NULL; }
            EastValue *v = beast2_decode_value(data, len, offset, val_type, ctx);
            if (!v) { east_value_release(k); east_value_release(dict); return NULL; }
            east_dict_set(dict, k, v);
            east_value_release(k);
            east_value_release(v);
        }
        return dict;
    }

    case EAST_TYPE_STRUCT: {
        size_t nf = type->data.struct_.num_fields;
        const char **names = malloc(nf * sizeof(char *));
        EastValue **values = malloc(nf * sizeof(EastValue *));
        if (!names || !values) {
            free(names);
            free(values);
            return NULL;
        }

        for (size_t i = 0; i < nf; i++) {
            names[i] = type->data.struct_.fields[i].name;
            EastType *ftype = type->data.struct_.fields[i].type;
            values[i] = beast2_decode_value(data, len, offset, ftype, ctx);
            if (!values[i]) {
                for (size_t j = 0; j < i; j++) {
                    east_value_release(values[j]);
                }
                free(names);
                free(values);
                return NULL;
            }
        }

        EastValue *result = east_struct_new(names, values, nf, type);
        for (size_t i = 0; i < nf; i++) {
            east_value_release(values[i]);
        }
        free(names);
        free(values);
        return result;
    }

    case EAST_TYPE_VARIANT: {
        uint64_t case_idx = read_varint(data, offset);
        if (case_idx >= type->data.variant.num_cases) return NULL;

        const char *case_name = type->data.variant.cases[case_idx].name;
        EastType *case_type = type->data.variant.cases[case_idx].type;

        EastValue *case_value = beast2_decode_value(data, len, offset, case_type, ctx);
        if (!case_value) return NULL;

        EastValue *result = east_variant_new(case_name, case_value, type);
        east_value_release(case_value);
        return result;
    }

    case EAST_TYPE_REF: {
        /* Ref also uses backreference protocol */
        size_t pre_offset = *offset;
        uint64_t distance = read_varint(data, offset);
        if (distance > 0) {
            size_t ref_off = pre_offset - distance;
            EastValue *ref = beast2_dec_ctx_find(ctx, ref_off);
            if (ref) { east_value_retain(ref); return ref; }
            return NULL;
        }
        size_t content_off = *offset;

        EastType *inner_type = type->data.element;
        EastValue *inner = beast2_decode_value(data, len, offset, inner_type, ctx);
        if (!inner) return NULL;
        EastValue *ref = east_ref_new(inner);
        east_value_release(inner);

        beast2_dec_ctx_add(ctx, ref, content_off);
        return ref;
    }

    case EAST_TYPE_VECTOR: {
        EastType *elem_type = type->data.element;
        uint64_t vlen = read_varint(data, offset);

        EastValue *vec = east_vector_new(elem_type, (size_t)vlen);
        if (!vec) return NULL;

        size_t elem_size = 0;
        if (elem_type->kind == EAST_TYPE_FLOAT) {
            elem_size = sizeof(double);
        } else if (elem_type->kind == EAST_TYPE_INTEGER) {
            elem_size = sizeof(int64_t);
        } else if (elem_type->kind == EAST_TYPE_BOOLEAN) {
            elem_size = sizeof(bool);
        }

        size_t byte_count = (size_t)vlen * elem_size;
        if (*offset + byte_count > len) {
            east_value_release(vec);
            return NULL;
        }
        memcpy(vec->data.vector.data, data + *offset, byte_count);
        *offset += byte_count;
        return vec;
    }

    case EAST_TYPE_MATRIX: {
        EastType *elem_type = type->data.element;
        uint64_t rows = read_varint(data, offset);
        uint64_t cols = read_varint(data, offset);

        EastValue *mat = east_matrix_new(elem_type, (size_t)rows, (size_t)cols);
        if (!mat) return NULL;

        size_t elem_size = 0;
        if (elem_type->kind == EAST_TYPE_FLOAT) {
            elem_size = sizeof(double);
        } else if (elem_type->kind == EAST_TYPE_INTEGER) {
            elem_size = sizeof(int64_t);
        } else if (elem_type->kind == EAST_TYPE_BOOLEAN) {
            elem_size = sizeof(bool);
        }

        size_t byte_count = (size_t)(rows * cols) * elem_size;
        if (*offset + byte_count > len) {
            east_value_release(mat);
            return NULL;
        }
        memcpy(mat->data.matrix.data, data + *offset, byte_count);
        *offset += byte_count;
        return mat;
    }

    case EAST_TYPE_RECURSIVE:
        if (type->data.recursive.node) {
            return beast2_decode_value(data, len, offset, type->data.recursive.node, ctx);
        }
        return NULL;

    case EAST_TYPE_FUNCTION:
    case EAST_TYPE_ASYNC_FUNCTION: {
        /* Ensure IR type is initialized */
        if (!east_ir_type) east_type_of_type_init();

        /* 1. Decode IR variant value */
        EastValue *ir_value = beast2_decode_value(data, len, offset, east_ir_type, ctx);
        if (!ir_value) return NULL;

        /* 2. Extract captures array from decoded IR */
        EastValue *fn_struct = ir_value->data.variant.value;
        EastValue *caps_arr = east_struct_get_field(fn_struct, "captures");
        size_t ir_ncaps = (caps_arr && caps_arr->kind == EAST_VAL_ARRAY) ? caps_arr->data.array.len : 0;

        /* 3. Read capture count and validate */
        uint64_t ncaps = read_varint(data, offset);
        if (ncaps != ir_ncaps) {
            east_value_release(ir_value);
            return NULL;
        }

        /* 4. Create captures environment and decode each capture value */
        Environment *captures_env = env_new(NULL);

        for (uint64_t i = 0; i < ncaps; i++) {
            EastValue *cap_var = caps_arr->data.array.items[i];
            EastValue *cap_s = cap_var->data.variant.value;
            EastValue *name_v = east_struct_get_field(cap_s, "name");
            EastValue *type_v = east_struct_get_field(cap_s, "type");
            bool is_mutable = false;
            EastValue *mut_v = east_struct_get_field(cap_s, "mutable");
            if (mut_v && mut_v->kind == EAST_VAL_BOOLEAN) is_mutable = mut_v->data.boolean;

            const char *cap_name = name_v->data.string.data;
            EastType *cap_type = east_type_from_value(type_v);

            EastValue *cap_val = beast2_decode_value(data, len, offset, cap_type, ctx);
            if (cap_type) east_type_release(cap_type);
            if (!cap_val) {
                env_release(captures_env);
                east_value_release(ir_value);
                return NULL;
            }

            /* Store capture value directly in environment.
             * The C compiler uses env_update for mutable captures (no Ref
             * wrapping), so we store all captures the same way. */
            env_set(captures_env, cap_name, cap_val);
            east_value_release(cap_val);
        }

        /* 5. Convert decoded IR to IRNode */
        IRNode *ir_node = east_ir_from_value(ir_value);
        if (!ir_node) {
            env_release(captures_env);
            east_value_release(ir_value);
            return NULL;
        }

        /* 6. Build EastCompiledFn */
        EastCompiledFn *fn = calloc(1, sizeof(EastCompiledFn));
        if (!fn) {
            ir_node_release(ir_node);
            env_release(captures_env);
            east_value_release(ir_value);
            return NULL;
        }

        fn->ir = ir_node->data.function.body;
        ir_node_retain(fn->ir);
        fn->captures = captures_env;
        fn->num_params = ir_node->data.function.num_params;
        if (fn->num_params > 0) {
            fn->param_names = calloc(fn->num_params, sizeof(char *));
            for (size_t i = 0; i < fn->num_params; i++) {
                fn->param_names[i] = strdup(ir_node->data.function.params[i].name);
            }
        }
        fn->platform = east_current_platform();
        fn->builtins = east_current_builtins();
        fn->source_ir = ir_value; /* already retained from decode */

        ir_node_release(ir_node);

        EastValue *result = east_function_value(fn);
        return result;
    }
    }

    return NULL;
}

EastValue *east_beast2_decode(const uint8_t *data, size_t len, EastType *type)
{
    if (!data || !type) return NULL;
    size_t offset = 0;
    Beast2DecodeCtx ctx;
    beast2_dec_ctx_init(&ctx);
    EastValue *result = beast2_decode_value(data, len, &offset, type, &ctx);
    beast2_dec_ctx_free(&ctx);
    return result;
}

/* ================================================================== */
/*  BEAST2 Type Schema Encoding/Decoding                               */
/*                                                                     */
/*  The type schema in the full format is a beast2-encoded value of    */
/*  east_type_type (EastTypeType).  We use east_type_to_value to       */
/*  convert EastType* -> EastValue*, then encode/decode it with the    */
/*  standard beast2 value codec.  This matches the TypeScript impl.    */
/* ================================================================== */

/* ================================================================== */
/*  BEAST2 Full-Format Encode/Decode (header + type schema + value)    */
/* ================================================================== */

static const uint8_t BEAST2_MAGIC[8] = {
    0x89, 0x45, 0x61, 0x73, 0x74, 0x0D, 0x0A, 0x01
};

ByteBuffer *east_beast2_encode_full(EastValue *value, EastType *type)
{
    if (!value || !type) return NULL;

    /* Ensure type system is initialized */
    if (!east_type_type) east_type_of_type_init();

    ByteBuffer *buf = byte_buffer_new(256);
    if (!buf) return NULL;

    /* 1. Write magic bytes */
    byte_buffer_write_bytes(buf, BEAST2_MAGIC, 8);

    /* 2. Write type schema as a beast2-encoded EastTypeType value */
    EastValue *type_val = east_type_to_value(type);
    if (type_val) {
        Beast2EncodeCtx schema_ctx;
        beast2_enc_ctx_init(&schema_ctx);
        beast2_encode_value(buf, type_val, east_type_type, &schema_ctx);
        beast2_enc_ctx_free(&schema_ctx);
        east_value_release(type_val);
    }

    /* 3. Write value data */
    Beast2EncodeCtx ctx;
    beast2_enc_ctx_init(&ctx);
    beast2_encode_value(buf, value, type, &ctx);
    beast2_enc_ctx_free(&ctx);

    return buf;
}

EastValue *east_beast2_decode_full(const uint8_t *data, size_t len,
                                   EastType *type)
{
    if (!data || !type) return NULL;
    if (len < 8) return NULL;

    /* 1. Verify magic bytes */
    if (memcmp(data, BEAST2_MAGIC, 8) != 0) return NULL;

    /* Ensure type system is initialized */
    if (!east_type_type) east_type_of_type_init();

    size_t offset = 8;

    /* 2. Decode type schema (advances offset past the schema bytes).
     *    The schema is a beast2-encoded EastTypeType value. */
    Beast2DecodeCtx schema_ctx;
    beast2_dec_ctx_init(&schema_ctx);
    EastValue *schema_val = beast2_decode_value(data, len, &offset,
                                                 east_type_type, &schema_ctx);
    beast2_dec_ctx_free(&schema_ctx);
    if (schema_val) east_value_release(schema_val);
    else return NULL;

    /* 3. Decode value from remaining data using the provided type */
    Beast2DecodeCtx dctx;
    beast2_dec_ctx_init(&dctx);
    EastValue *result = beast2_decode_value(data, len, &offset, type, &dctx);
    beast2_dec_ctx_free(&dctx);
    return result;
}
