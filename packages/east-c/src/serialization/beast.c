/*
 * Beast v1 binary serialization for East types.
 *
 * Format: 8-byte magic header + type schema + value data.
 *
 * Uses "twiddled big-endian" encoding for integers and floats to enable
 * memcmp-based ordering of encoded values.
 *
 * Type schema encoding:
 *   Type bytes (single byte, NOT varint):
 *     0:Array  1:Blob  2:Boolean  3:DateTime  4:Dict  5:Float  6:Integer
 *     8:Null  9:Set  10:String  11:Struct  13:Variant
 *     (7 and 12 are reserved/unused)
 *
 * Value encoding:
 *   Null:     0 bytes
 *   Boolean:  1 byte (0x00 or 0x01)
 *   Integer:  twiddled big-endian int64 (XOR sign bit, big-endian)
 *   Float:    twiddled big-endian double (sign-dependent XOR, big-endian)
 *   String:   UTF-8 bytes + null terminator
 *   DateTime: twiddled big-endian int64 (same as Integer)
 *   Blob:     8-byte big-endian length + raw bytes
 *   Array:    continuation byte per element (0x01 + elem)* + 0x00
 *   Set:      continuation byte per element (0x01 + elem)* + 0x00
 *   Dict:     continuation byte per entry (0x01 + key + val)* + 0x00
 *   Struct:   fields in schema order, no separators
 *   Variant:  1-byte case index + case value
 */

#include "east/serialization.h"
#include "east/types.h"
#include "east/values.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ================================================================== */
/*  Magic header                                                       */
/* ================================================================== */

static const uint8_t BEAST_MAGIC[8] = {
    0x45, 0x61, 0x73, 0x74, 0x00, 0xEA, 0x57, 0xFF
};

/* ================================================================== */
/*  Beast v1 Type Schema Encoding                                      */
/* ================================================================== */

/*
 * Beast v1 type byte tags (single byte, NOT varint):
 *   0:Array  1:Blob  2:Boolean  3:DateTime  4:Dict  5:Float  6:Integer
 *   8:Null  9:Set  10:String  11:Struct  13:Variant
 */

#define BEAST_TYPE_ARRAY    0
#define BEAST_TYPE_BLOB     1
#define BEAST_TYPE_BOOLEAN  2
#define BEAST_TYPE_DATETIME 3
#define BEAST_TYPE_DICT     4
#define BEAST_TYPE_FLOAT    5
#define BEAST_TYPE_INTEGER  6
/* 7 is reserved */
#define BEAST_TYPE_NULL     8
#define BEAST_TYPE_SET      9
#define BEAST_TYPE_STRING   10
#define BEAST_TYPE_STRUCT   11
/* 12 is reserved */
#define BEAST_TYPE_VARIANT  13

static void beast_encode_type(ByteBuffer *buf, EastType *type)
{
    if (!type) return;

    switch (type->kind) {
    case EAST_TYPE_NULL:
        byte_buffer_write_u8(buf, BEAST_TYPE_NULL);
        break;

    case EAST_TYPE_BOOLEAN:
        byte_buffer_write_u8(buf, BEAST_TYPE_BOOLEAN);
        break;

    case EAST_TYPE_INTEGER:
        byte_buffer_write_u8(buf, BEAST_TYPE_INTEGER);
        break;

    case EAST_TYPE_FLOAT:
        byte_buffer_write_u8(buf, BEAST_TYPE_FLOAT);
        break;

    case EAST_TYPE_STRING:
        byte_buffer_write_u8(buf, BEAST_TYPE_STRING);
        break;

    case EAST_TYPE_DATETIME:
        byte_buffer_write_u8(buf, BEAST_TYPE_DATETIME);
        break;

    case EAST_TYPE_BLOB:
        byte_buffer_write_u8(buf, BEAST_TYPE_BLOB);
        break;

    case EAST_TYPE_ARRAY:
        byte_buffer_write_u8(buf, BEAST_TYPE_ARRAY);
        beast_encode_type(buf, type->data.element);
        break;

    case EAST_TYPE_SET:
        byte_buffer_write_u8(buf, BEAST_TYPE_SET);
        beast_encode_type(buf, type->data.element);
        break;

    case EAST_TYPE_DICT:
        byte_buffer_write_u8(buf, BEAST_TYPE_DICT);
        beast_encode_type(buf, type->data.dict.key);
        beast_encode_type(buf, type->data.dict.value);
        break;

    case EAST_TYPE_STRUCT: {
        byte_buffer_write_u8(buf, BEAST_TYPE_STRUCT);
        size_t nf = type->data.struct_.num_fields;
        for (size_t i = 0; i < nf; i++) {
            /* Field entry: 0x01 + null-terminated name + field type */
            byte_buffer_write_u8(buf, 0x01);
            const char *name = type->data.struct_.fields[i].name;
            size_t name_len = strlen(name);
            byte_buffer_write_bytes(buf, (const uint8_t *)name, name_len);
            byte_buffer_write_u8(buf, 0x00); /* null terminator for name */
            beast_encode_type(buf, type->data.struct_.fields[i].type);
        }
        /* Terminator byte */
        byte_buffer_write_u8(buf, 0x00);
        break;
    }

    case EAST_TYPE_VARIANT: {
        byte_buffer_write_u8(buf, BEAST_TYPE_VARIANT);
        size_t nc = type->data.variant.num_cases;
        for (size_t i = 0; i < nc; i++) {
            /* Case entry: 0x01 + null-terminated name + case type */
            byte_buffer_write_u8(buf, 0x01);
            const char *name = type->data.variant.cases[i].name;
            size_t name_len = strlen(name);
            byte_buffer_write_bytes(buf, (const uint8_t *)name, name_len);
            byte_buffer_write_u8(buf, 0x00); /* null terminator for name */
            beast_encode_type(buf, type->data.variant.cases[i].type);
        }
        /* Terminator byte */
        byte_buffer_write_u8(buf, 0x00);
        break;
    }

    /* Unsupported types in Beast v1: treat as Null */
    case EAST_TYPE_NEVER:
    case EAST_TYPE_REF:
    case EAST_TYPE_VECTOR:
    case EAST_TYPE_MATRIX:
    case EAST_TYPE_FUNCTION:
    case EAST_TYPE_ASYNC_FUNCTION:
    case EAST_TYPE_RECURSIVE:
        byte_buffer_write_u8(buf, BEAST_TYPE_NULL);
        break;
    }
}

static EastType *beast_decode_type(const uint8_t *data, size_t len,
                                   size_t *offset)
{
    if (*offset >= len) return NULL;

    uint8_t tag = data[(*offset)++];

    switch (tag) {
    case BEAST_TYPE_NULL:
        east_type_retain(&east_null_type);
        return &east_null_type;

    case BEAST_TYPE_BOOLEAN:
        east_type_retain(&east_boolean_type);
        return &east_boolean_type;

    case BEAST_TYPE_INTEGER:
        east_type_retain(&east_integer_type);
        return &east_integer_type;

    case BEAST_TYPE_FLOAT:
        east_type_retain(&east_float_type);
        return &east_float_type;

    case BEAST_TYPE_STRING:
        east_type_retain(&east_string_type);
        return &east_string_type;

    case BEAST_TYPE_DATETIME:
        east_type_retain(&east_datetime_type);
        return &east_datetime_type;

    case BEAST_TYPE_BLOB:
        east_type_retain(&east_blob_type);
        return &east_blob_type;

    case BEAST_TYPE_ARRAY: {
        EastType *elem = beast_decode_type(data, len, offset);
        if (!elem) return NULL;
        EastType *t = east_array_type(elem);
        east_type_release(elem);
        return t;
    }

    case BEAST_TYPE_SET: {
        EastType *elem = beast_decode_type(data, len, offset);
        if (!elem) return NULL;
        EastType *t = east_set_type(elem);
        east_type_release(elem);
        return t;
    }

    case BEAST_TYPE_DICT: {
        EastType *key = beast_decode_type(data, len, offset);
        if (!key) return NULL;
        EastType *val = beast_decode_type(data, len, offset);
        if (!val) { east_type_release(key); return NULL; }
        EastType *t = east_dict_type(key, val);
        east_type_release(key);
        east_type_release(val);
        return t;
    }

    case BEAST_TYPE_STRUCT: {
        /* Read fields until we see 0x00 terminator */
        size_t cap = 8;
        size_t count = 0;
        const char **names = malloc(cap * sizeof(char *));
        EastType **types = malloc(cap * sizeof(EastType *));
        if (!names || !types) {
            free(names);
            free(types);
            return NULL;
        }

        while (*offset < len && data[*offset] == 0x01) {
            (*offset)++; /* consume 0x01 */

            /* Read null-terminated field name */
            size_t name_start = *offset;
            while (*offset < len && data[*offset] != 0x00) {
                (*offset)++;
            }
            if (*offset >= len) {
                /* Unterminated name */
                for (size_t i = 0; i < count; i++) {
                    free((char *)names[i]);
                    east_type_release(types[i]);
                }
                free(names);
                free(types);
                return NULL;
            }
            size_t name_len = *offset - name_start;
            (*offset)++; /* consume null terminator */

            char *name = malloc(name_len + 1);
            if (!name) {
                for (size_t i = 0; i < count; i++) {
                    free((char *)names[i]);
                    east_type_release(types[i]);
                }
                free(names);
                free(types);
                return NULL;
            }
            memcpy(name, data + name_start, name_len);
            name[name_len] = '\0';

            /* Decode field type */
            EastType *ftype = beast_decode_type(data, len, offset);
            if (!ftype) {
                free(name);
                for (size_t i = 0; i < count; i++) {
                    free((char *)names[i]);
                    east_type_release(types[i]);
                }
                free(names);
                free(types);
                return NULL;
            }

            /* Grow arrays if needed */
            if (count >= cap) {
                cap *= 2;
                const char **new_names = realloc(names, cap * sizeof(char *));
                EastType **new_types = realloc(types, cap * sizeof(EastType *));
                if (!new_names || !new_types) {
                    free(name);
                    east_type_release(ftype);
                    for (size_t i = 0; i < count; i++) {
                        free((char *)names[i]);
                        east_type_release(types[i]);
                    }
                    if (new_names) free(new_names); else free(names);
                    if (new_types) free(new_types); else free(types);
                    return NULL;
                }
                names = new_names;
                types = new_types;
            }

            names[count] = name;
            types[count] = ftype;
            count++;
        }

        /* Consume 0x00 terminator */
        if (*offset < len && data[*offset] == 0x00) {
            (*offset)++;
        }

        EastType *t = east_struct_type(names, types, count);
        for (size_t i = 0; i < count; i++) {
            free((char *)names[i]);
            east_type_release(types[i]);
        }
        free(names);
        free(types);
        return t;
    }

    case BEAST_TYPE_VARIANT: {
        /* Read cases until we see 0x00 terminator */
        size_t cap = 8;
        size_t count = 0;
        const char **names = malloc(cap * sizeof(char *));
        EastType **types = malloc(cap * sizeof(EastType *));
        if (!names || !types) {
            free(names);
            free(types);
            return NULL;
        }

        while (*offset < len && data[*offset] == 0x01) {
            (*offset)++; /* consume 0x01 */

            /* Read null-terminated case name */
            size_t name_start = *offset;
            while (*offset < len && data[*offset] != 0x00) {
                (*offset)++;
            }
            if (*offset >= len) {
                for (size_t i = 0; i < count; i++) {
                    free((char *)names[i]);
                    east_type_release(types[i]);
                }
                free(names);
                free(types);
                return NULL;
            }
            size_t name_len = *offset - name_start;
            (*offset)++; /* consume null terminator */

            char *name = malloc(name_len + 1);
            if (!name) {
                for (size_t i = 0; i < count; i++) {
                    free((char *)names[i]);
                    east_type_release(types[i]);
                }
                free(names);
                free(types);
                return NULL;
            }
            memcpy(name, data + name_start, name_len);
            name[name_len] = '\0';

            /* Decode case type */
            EastType *ctype = beast_decode_type(data, len, offset);
            if (!ctype) {
                free(name);
                for (size_t i = 0; i < count; i++) {
                    free((char *)names[i]);
                    east_type_release(types[i]);
                }
                free(names);
                free(types);
                return NULL;
            }

            /* Grow arrays if needed */
            if (count >= cap) {
                cap *= 2;
                const char **new_names = realloc(names, cap * sizeof(char *));
                EastType **new_types = realloc(types, cap * sizeof(EastType *));
                if (!new_names || !new_types) {
                    free(name);
                    east_type_release(ctype);
                    for (size_t i = 0; i < count; i++) {
                        free((char *)names[i]);
                        east_type_release(types[i]);
                    }
                    if (new_names) free(new_names); else free(names);
                    if (new_types) free(new_types); else free(types);
                    return NULL;
                }
                names = new_names;
                types = new_types;
            }

            names[count] = name;
            types[count] = ctype;
            count++;
        }

        /* Consume 0x00 terminator */
        if (*offset < len && data[*offset] == 0x00) {
            (*offset)++;
        }

        EastType *t = east_variant_type(names, types, count);
        for (size_t i = 0; i < count; i++) {
            free((char *)names[i]);
            east_type_release(types[i]);
        }
        free(names);
        free(types);
        return t;
    }

    default:
        return NULL;
    }
}

/* ================================================================== */
/*  Twiddled Big-Endian Helpers                                        */
/* ================================================================== */

/*
 * Write a twiddled big-endian int64:
 *   1. XOR with 0x8000000000000000 (flip sign bit)
 *   2. Write 8 bytes in big-endian order
 */
static void write_twiddled_int64(ByteBuffer *buf, int64_t val)
{
    uint64_t u = (uint64_t)val ^ UINT64_C(0x8000000000000000);
    uint8_t bytes[8];
    bytes[0] = (uint8_t)(u >> 56);
    bytes[1] = (uint8_t)(u >> 48);
    bytes[2] = (uint8_t)(u >> 40);
    bytes[3] = (uint8_t)(u >> 32);
    bytes[4] = (uint8_t)(u >> 24);
    bytes[5] = (uint8_t)(u >> 16);
    bytes[6] = (uint8_t)(u >> 8);
    bytes[7] = (uint8_t)(u);
    byte_buffer_write_bytes(buf, bytes, 8);
}

/*
 * Read a twiddled big-endian int64:
 *   1. Read 8 bytes big-endian as uint64
 *   2. XOR with 0x8000000000000000
 */
static int64_t read_twiddled_int64(const uint8_t *data, size_t *offset)
{
    uint64_t u = 0;
    u |= (uint64_t)data[*offset + 0] << 56;
    u |= (uint64_t)data[*offset + 1] << 48;
    u |= (uint64_t)data[*offset + 2] << 40;
    u |= (uint64_t)data[*offset + 3] << 32;
    u |= (uint64_t)data[*offset + 4] << 24;
    u |= (uint64_t)data[*offset + 5] << 16;
    u |= (uint64_t)data[*offset + 6] << 8;
    u |= (uint64_t)data[*offset + 7];
    *offset += 8;
    return (int64_t)(u ^ UINT64_C(0x8000000000000000));
}

/*
 * Write a twiddled big-endian double:
 *   1. Interpret IEEE 754 bits as uint64
 *   2. If sign bit 0 (positive/+0): XOR with 0x8000000000000000
 *   3. If sign bit 1 (negative/-0): flip ALL bits
 *   4. Write 8 bytes big-endian
 */
static void write_twiddled_float64(ByteBuffer *buf, double val)
{
    uint64_t u;
    memcpy(&u, &val, sizeof(u));

    if (u & UINT64_C(0x8000000000000000)) {
        /* Sign bit is set (negative) -- flip all bits */
        u = ~u;
    } else {
        /* Sign bit is clear (positive) -- flip sign bit only */
        u ^= UINT64_C(0x8000000000000000);
    }

    uint8_t bytes[8];
    bytes[0] = (uint8_t)(u >> 56);
    bytes[1] = (uint8_t)(u >> 48);
    bytes[2] = (uint8_t)(u >> 40);
    bytes[3] = (uint8_t)(u >> 32);
    bytes[4] = (uint8_t)(u >> 24);
    bytes[5] = (uint8_t)(u >> 16);
    bytes[6] = (uint8_t)(u >> 8);
    bytes[7] = (uint8_t)(u);
    byte_buffer_write_bytes(buf, bytes, 8);
}

/*
 * Read a twiddled big-endian double:
 *   1. Read 8 bytes big-endian as uint64
 *   2. If bit 63 is set (>= 0x8000000000000000): XOR with 0x8000000000000000
 *   3. Otherwise: flip ALL bits
 *   4. Interpret as double
 */
static double read_twiddled_float64(const uint8_t *data, size_t *offset)
{
    uint64_t u = 0;
    u |= (uint64_t)data[*offset + 0] << 56;
    u |= (uint64_t)data[*offset + 1] << 48;
    u |= (uint64_t)data[*offset + 2] << 40;
    u |= (uint64_t)data[*offset + 3] << 32;
    u |= (uint64_t)data[*offset + 4] << 24;
    u |= (uint64_t)data[*offset + 5] << 16;
    u |= (uint64_t)data[*offset + 6] << 8;
    u |= (uint64_t)data[*offset + 7];
    *offset += 8;

    if (u & UINT64_C(0x8000000000000000)) {
        /* Bit 63 is set: XOR with 0x8000000000000000 */
        u ^= UINT64_C(0x8000000000000000);
    } else {
        /* Bit 63 is clear: flip all bits */
        u = ~u;
    }

    double val;
    memcpy(&val, &u, sizeof(val));
    return val;
}

/*
 * Write a big-endian uint64 (for blob length).
 */
static void write_be_uint64(ByteBuffer *buf, uint64_t val)
{
    uint8_t bytes[8];
    bytes[0] = (uint8_t)(val >> 56);
    bytes[1] = (uint8_t)(val >> 48);
    bytes[2] = (uint8_t)(val >> 40);
    bytes[3] = (uint8_t)(val >> 32);
    bytes[4] = (uint8_t)(val >> 24);
    bytes[5] = (uint8_t)(val >> 16);
    bytes[6] = (uint8_t)(val >> 8);
    bytes[7] = (uint8_t)(val);
    byte_buffer_write_bytes(buf, bytes, 8);
}

/*
 * Read a big-endian uint64.
 */
static uint64_t read_be_uint64(const uint8_t *data, size_t *offset)
{
    uint64_t val = 0;
    val |= (uint64_t)data[*offset + 0] << 56;
    val |= (uint64_t)data[*offset + 1] << 48;
    val |= (uint64_t)data[*offset + 2] << 40;
    val |= (uint64_t)data[*offset + 3] << 32;
    val |= (uint64_t)data[*offset + 4] << 24;
    val |= (uint64_t)data[*offset + 5] << 16;
    val |= (uint64_t)data[*offset + 6] << 8;
    val |= (uint64_t)data[*offset + 7];
    *offset += 8;
    return val;
}

/* ================================================================== */
/*  Beast v1 Value Encoding                                            */
/* ================================================================== */

static void beast_encode_value(ByteBuffer *buf, EastValue *value,
                               EastType *type)
{
    if (!type) return;

    switch (type->kind) {
    case EAST_TYPE_NULL:
        /* Null encodes as 0 bytes */
        break;

    case EAST_TYPE_BOOLEAN:
        byte_buffer_write_u8(buf, value->data.boolean ? 0x01 : 0x00);
        break;

    case EAST_TYPE_INTEGER:
        write_twiddled_int64(buf, value->data.integer);
        break;

    case EAST_TYPE_FLOAT:
        write_twiddled_float64(buf, value->data.float64);
        break;

    case EAST_TYPE_STRING: {
        /* UTF-8 bytes followed by null terminator */
        size_t slen = value->data.string.len;
        byte_buffer_write_bytes(buf, (const uint8_t *)value->data.string.data,
                                slen);
        byte_buffer_write_u8(buf, 0x00);
        break;
    }

    case EAST_TYPE_DATETIME:
        /* Same as integer: twiddled big-endian int64 (epoch millis) */
        write_twiddled_int64(buf, value->data.datetime);
        break;

    case EAST_TYPE_BLOB: {
        /* 8-byte big-endian length + raw bytes */
        size_t blen = value->data.blob.len;
        write_be_uint64(buf, (uint64_t)blen);
        byte_buffer_write_bytes(buf, value->data.blob.data, blen);
        break;
    }

    case EAST_TYPE_ARRAY: {
        /* Continuation byte pattern: (0x01 + elem)* + 0x00 */
        EastType *elem_type = type->data.element;
        size_t count = value->data.array.len;
        for (size_t i = 0; i < count; i++) {
            byte_buffer_write_u8(buf, 0x01);
            beast_encode_value(buf, value->data.array.items[i], elem_type);
        }
        byte_buffer_write_u8(buf, 0x00);
        break;
    }

    case EAST_TYPE_SET: {
        /* Continuation byte pattern: (0x01 + elem)* + 0x00 */
        EastType *elem_type = type->data.element;
        size_t count = value->data.set.len;
        for (size_t i = 0; i < count; i++) {
            byte_buffer_write_u8(buf, 0x01);
            beast_encode_value(buf, value->data.set.items[i], elem_type);
        }
        byte_buffer_write_u8(buf, 0x00);
        break;
    }

    case EAST_TYPE_DICT: {
        /* Continuation byte pattern: (0x01 + key + val)* + 0x00 */
        EastType *key_type = type->data.dict.key;
        EastType *val_type = type->data.dict.value;
        size_t count = value->data.dict.len;
        for (size_t i = 0; i < count; i++) {
            byte_buffer_write_u8(buf, 0x01);
            beast_encode_value(buf, value->data.dict.keys[i], key_type);
            beast_encode_value(buf, value->data.dict.values[i], val_type);
        }
        byte_buffer_write_u8(buf, 0x00);
        break;
    }

    case EAST_TYPE_STRUCT: {
        /* Fields encoded sequentially in schema order */
        size_t nf = type->data.struct_.num_fields;
        /* Struct values always have fields in type schema order */
        for (size_t i = 0; i < nf; i++) {
            EastType *ftype = type->data.struct_.fields[i].type;
            EastValue *fval = (value->kind == EAST_VAL_STRUCT && i < value->data.struct_.num_fields)
                            ? value->data.struct_.field_values[i] : NULL;
            if (fval) {
                beast_encode_value(buf, fval, ftype);
            } else {
                /* Missing field -- encode a null placeholder */
                EastValue *null_val = east_null();
                beast_encode_value(buf, null_val, ftype);
                east_value_release(null_val);
            }
        }
        break;
    }

    case EAST_TYPE_VARIANT: {
        /* 1 byte: case index (0-based), then case value */
        const char *case_name = value->data.variant.case_name;
        for (size_t i = 0; i < type->data.variant.num_cases; i++) {
            if (strcmp(type->data.variant.cases[i].name, case_name) == 0) {
                byte_buffer_write_u8(buf, (uint8_t)i);
                beast_encode_value(buf, value->data.variant.value,
                                   type->data.variant.cases[i].type);
                break;
            }
        }
        break;
    }

    /* Unsupported types -- write nothing */
    case EAST_TYPE_NEVER:
    case EAST_TYPE_REF:
    case EAST_TYPE_VECTOR:
    case EAST_TYPE_MATRIX:
    case EAST_TYPE_FUNCTION:
    case EAST_TYPE_ASYNC_FUNCTION:
    case EAST_TYPE_RECURSIVE:
        break;
    }
}

/* ================================================================== */
/*  Beast v1 Value Decoding                                            */
/* ================================================================== */

static EastValue *beast_decode_value(const uint8_t *data, size_t len,
                                     size_t *offset, EastType *type)
{
    if (!type) return NULL;

    switch (type->kind) {
    case EAST_TYPE_NULL:
        /* 0 bytes */
        return east_null();

    case EAST_TYPE_BOOLEAN: {
        if (*offset >= len) return NULL;
        bool val = data[(*offset)++] != 0;
        return east_boolean(val);
    }

    case EAST_TYPE_INTEGER: {
        if (*offset + 8 > len) return NULL;
        int64_t val = read_twiddled_int64(data, offset);
        return east_integer(val);
    }

    case EAST_TYPE_FLOAT: {
        if (*offset + 8 > len) return NULL;
        double val = read_twiddled_float64(data, offset);
        return east_float(val);
    }

    case EAST_TYPE_STRING: {
        /* Read UTF-8 bytes until null terminator */
        size_t start = *offset;
        while (*offset < len && data[*offset] != 0x00) {
            (*offset)++;
        }
        if (*offset >= len) return NULL; /* no null terminator found */
        size_t slen = *offset - start;
        (*offset)++; /* consume null terminator */
        return east_string_len((const char *)(data + start), slen);
    }

    case EAST_TYPE_DATETIME: {
        if (*offset + 8 > len) return NULL;
        int64_t millis = read_twiddled_int64(data, offset);
        return east_datetime(millis);
    }

    case EAST_TYPE_BLOB: {
        if (*offset + 8 > len) return NULL;
        uint64_t blen = read_be_uint64(data, offset);
        if (*offset + blen > len) return NULL;
        EastValue *val = east_blob(data + *offset, (size_t)blen);
        *offset += (size_t)blen;
        return val;
    }

    case EAST_TYPE_ARRAY: {
        /* Continuation byte pattern: (0x01 + elem)* + 0x00 */
        EastType *elem_type = type->data.element;
        EastValue *arr = east_array_new(elem_type);
        if (!arr) return NULL;

        while (*offset < len && data[*offset] == 0x01) {
            (*offset)++; /* consume 0x01 continuation byte */
            EastValue *elem = beast_decode_value(data, len, offset, elem_type);
            if (!elem) { east_value_release(arr); return NULL; }
            east_array_push(arr, elem);
            east_value_release(elem);
        }
        /* Consume 0x00 terminator */
        if (*offset < len && data[*offset] == 0x00) {
            (*offset)++;
        }
        return arr;
    }

    case EAST_TYPE_SET: {
        /* Continuation byte pattern: (0x01 + elem)* + 0x00 */
        EastType *elem_type = type->data.element;
        EastValue *set = east_set_new(elem_type);
        if (!set) return NULL;

        while (*offset < len && data[*offset] == 0x01) {
            (*offset)++; /* consume 0x01 continuation byte */
            EastValue *elem = beast_decode_value(data, len, offset, elem_type);
            if (!elem) { east_value_release(set); return NULL; }
            east_set_insert(set, elem);
            east_value_release(elem);
        }
        /* Consume 0x00 terminator */
        if (*offset < len && data[*offset] == 0x00) {
            (*offset)++;
        }
        return set;
    }

    case EAST_TYPE_DICT: {
        /* Continuation byte pattern: (0x01 + key + val)* + 0x00 */
        EastType *key_type = type->data.dict.key;
        EastType *val_type = type->data.dict.value;
        EastValue *dict = east_dict_new(key_type, val_type);
        if (!dict) return NULL;

        while (*offset < len && data[*offset] == 0x01) {
            (*offset)++; /* consume 0x01 continuation byte */
            EastValue *k = beast_decode_value(data, len, offset, key_type);
            if (!k) { east_value_release(dict); return NULL; }
            EastValue *v = beast_decode_value(data, len, offset, val_type);
            if (!v) {
                east_value_release(k);
                east_value_release(dict);
                return NULL;
            }
            east_dict_set(dict, k, v);
            east_value_release(k);
            east_value_release(v);
        }
        /* Consume 0x00 terminator */
        if (*offset < len && data[*offset] == 0x00) {
            (*offset)++;
        }
        return dict;
    }

    case EAST_TYPE_STRUCT: {
        /* Fields decoded sequentially in schema order */
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
            values[i] = beast_decode_value(data, len, offset, ftype);
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
        /* 1 byte: case index, then case value */
        if (*offset >= len) return NULL;
        uint8_t case_idx = data[(*offset)++];
        if (case_idx >= type->data.variant.num_cases) return NULL;

        const char *case_name = type->data.variant.cases[case_idx].name;
        EastType *case_type = type->data.variant.cases[case_idx].type;

        EastValue *case_value = beast_decode_value(data, len, offset,
                                                   case_type);
        if (!case_value) return NULL;

        EastValue *result = east_variant_new(case_name, case_value, type);
        east_value_release(case_value);
        return result;
    }

    /* Unsupported types -- return null */
    case EAST_TYPE_NEVER:
    case EAST_TYPE_REF:
    case EAST_TYPE_VECTOR:
    case EAST_TYPE_MATRIX:
    case EAST_TYPE_FUNCTION:
    case EAST_TYPE_ASYNC_FUNCTION:
    case EAST_TYPE_RECURSIVE:
        return east_null();
    }

    return NULL;
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

ByteBuffer *east_beast_encode(EastValue *value, EastType *type)
{
    if (!value || !type) return NULL;

    ByteBuffer *buf = byte_buffer_new(256);
    if (!buf) return NULL;

    /* 1. Write magic header */
    byte_buffer_write_bytes(buf, BEAST_MAGIC, 8);

    /* 2. Write type schema */
    beast_encode_type(buf, type);

    /* 3. Write value data */
    beast_encode_value(buf, value, type);

    return buf;
}

EastValue *east_beast_decode(const uint8_t *data, size_t len, EastType *type)
{
    if (!data || !type) return NULL;
    if (len < 8) return NULL;

    /* 1. Verify magic header */
    if (memcmp(data, BEAST_MAGIC, 8) != 0) return NULL;

    size_t offset = 8;

    /* 2. Skip type schema (decode to advance offset, then discard) */
    EastType *decoded_type = beast_decode_type(data, len, &offset);
    if (decoded_type) {
        east_type_release(decoded_type);
    }

    /* 3. Decode value using the provided type */
    return beast_decode_value(data, len, &offset, type);
}
