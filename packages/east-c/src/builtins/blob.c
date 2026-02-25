/*
 * Blob builtin functions.
 */
#include "east/builtins.h"
#include "east/serialization.h"
#include "east/values.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- static type context for serialization builtins --- */

/*
 * The compiler evaluates arguments BEFORE calling the factory, so the
 * factory call and the impl call are guaranteed to be adjacent (no IR
 * evaluation between them).  This makes a simple static safe.
 */
static _Thread_local EastType *beast_type_ctx = NULL;
static _Thread_local EastType *beast2_type_ctx = NULL;
static _Thread_local EastType *csv_struct_type_ctx = NULL;

/* --- static implementations --- */

static EastValue *blob_size(EastValue **args, size_t n) {
    (void)n;
    return east_integer((int64_t)args[0]->data.blob.len);
}

static EastValue *blob_get_uint8(EastValue **args, size_t n) {
    (void)n;
    int64_t index = args[1]->data.integer;
    size_t len = args[0]->data.blob.len;
    if (index < 0 || (size_t)index >= len) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Blob index %lld out of bounds",
                 (long long)index);
        east_builtin_error(msg);
        return NULL;
    }
    return east_integer((int64_t)args[0]->data.blob.data[(size_t)index]);
}

static EastValue *blob_decode_utf8(EastValue **args, size_t n) {
    (void)n;
    return east_string_len((const char *)args[0]->data.blob.data, args[0]->data.blob.len);
}

static EastValue *blob_decode_utf16(EastValue **args, size_t n) {
    (void)n;
    const uint8_t *data = args[0]->data.blob.data;
    size_t len = args[0]->data.blob.len;

    /* Skip BOM if present */
    size_t start = 0;
    bool big_endian = false;
    if (len >= 2) {
        if (data[0] == 0xFF && data[1] == 0xFE) {
            start = 2; /* UTF-16LE BOM */
        } else if (data[0] == 0xFE && data[1] == 0xFF) {
            start = 2;
            big_endian = true; /* UTF-16BE BOM */
        }
    }

    size_t chars = (len - start) / 2;
    char *buf = malloc(chars * 4 + 1); /* worst case UTF-8 */
    if (!buf) return east_string("");
    size_t out = 0;
    for (size_t i = start; i + 1 < len; i += 2) {
        uint16_t cp;
        if (big_endian) {
            cp = (uint16_t)((data[i] << 8) | data[i+1]);
        } else {
            cp = (uint16_t)(data[i] | (data[i+1] << 8));
        }

        /* Handle surrogate pairs */
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < len) {
            uint16_t lo;
            if (big_endian) {
                lo = (uint16_t)((data[i+2] << 8) | data[i+3]);
            } else {
                lo = (uint16_t)(data[i+2] | (data[i+3] << 8));
            }
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                uint32_t full = 0x10000 + ((uint32_t)(cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2; /* skip the low surrogate */
                /* Encode as 4-byte UTF-8 */
                buf[out++] = (char)(0xF0 | (full >> 18));
                buf[out++] = (char)(0x80 | ((full >> 12) & 0x3F));
                buf[out++] = (char)(0x80 | ((full >> 6) & 0x3F));
                buf[out++] = (char)(0x80 | (full & 0x3F));
                continue;
            }
        }

        if (cp < 0x80) {
            buf[out++] = (char)cp;
        } else if (cp < 0x800) {
            buf[out++] = (char)(0xC0 | (cp >> 6));
            buf[out++] = (char)(0x80 | (cp & 0x3F));
        } else {
            buf[out++] = (char)(0xE0 | (cp >> 12));
            buf[out++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[out++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    buf[out] = '\0';
    EastValue *result = east_string_len(buf, out);
    free(buf);
    return result;
}

static EastValue *string_encode_utf8(EastValue **args, size_t n) {
    (void)n;
    return east_blob((const uint8_t *)args[0]->data.string.data, args[0]->data.string.len);
}

static EastValue *string_encode_utf16(EastValue **args, size_t n) {
    (void)n;
    /*
     * UTF-16LE encode from UTF-8 string, with BOM prefix (0xFF 0xFE).
     * Handles full Unicode including surrogate pairs for codepoints > 0xFFFF.
     */
    const uint8_t *s = (const uint8_t *)args[0]->data.string.data;
    size_t len = args[0]->data.string.len;

    /* Worst case: BOM (2 bytes) + each UTF-8 char becomes surrogate pair (4 bytes) */
    size_t max_out = 2 + len * 4;
    uint8_t *buf = malloc(max_out);
    if (!buf) return east_blob(NULL, 0);

    /* Write BOM (UTF-16LE: 0xFF 0xFE) */
    buf[0] = 0xFF;
    buf[1] = 0xFE;
    size_t out = 2;

    size_t i = 0;
    while (i < len) {
        uint32_t cp;
        uint8_t b = s[i];
        if (b < 0x80) {
            cp = b;
            i += 1;
        } else if ((b & 0xE0) == 0xC0) {
            cp = (b & 0x1F) << 6;
            if (i + 1 < len) cp |= (s[i+1] & 0x3F);
            i += 2;
        } else if ((b & 0xF0) == 0xE0) {
            cp = (b & 0x0F) << 12;
            if (i + 1 < len) cp |= (uint32_t)(s[i+1] & 0x3F) << 6;
            if (i + 2 < len) cp |= (s[i+2] & 0x3F);
            i += 3;
        } else if ((b & 0xF8) == 0xF0) {
            cp = (b & 0x07) << 18;
            if (i + 1 < len) cp |= (uint32_t)(s[i+1] & 0x3F) << 12;
            if (i + 2 < len) cp |= (uint32_t)(s[i+2] & 0x3F) << 6;
            if (i + 3 < len) cp |= (s[i+3] & 0x3F);
            i += 4;
        } else {
            cp = 0xFFFD; /* replacement character */
            i += 1;
        }

        if (cp >= 0x10000) {
            /* Surrogate pair */
            cp -= 0x10000;
            uint16_t hi = 0xD800 + (uint16_t)(cp >> 10);
            uint16_t lo = 0xDC00 + (uint16_t)(cp & 0x3FF);
            buf[out++] = (uint8_t)(hi & 0xFF);
            buf[out++] = (uint8_t)(hi >> 8);
            buf[out++] = (uint8_t)(lo & 0xFF);
            buf[out++] = (uint8_t)(lo >> 8);
        } else {
            buf[out++] = (uint8_t)(cp & 0xFF);
            buf[out++] = (uint8_t)(cp >> 8);
        }
    }

    EastValue *result = east_blob(buf, out);
    free(buf);
    return result;
}

/* --- Beast v1 encode/decode --- */

static EastValue *blob_encode_beast(EastValue **args, size_t n) {
    (void)n;
    EastType *type = beast_type_ctx;
    if (!type) {
        east_builtin_error("Beast encode: no type context");
        return NULL;
    }
    ByteBuffer *buf = east_beast_encode(args[0], type);
    if (!buf) return east_blob(NULL, 0);
    EastValue *result = east_blob(buf->data, buf->len);
    byte_buffer_free(buf);
    return result;
}

static EastValue *blob_decode_beast(EastValue **args, size_t n) {
    (void)n;
    EastType *type = beast_type_ctx;
    if (!type) {
        east_builtin_error("Beast decode: no type context");
        return NULL;
    }
    EastValue *result = east_beast_decode(
        args[0]->data.blob.data, args[0]->data.blob.len, type);
    if (!result) {
        east_builtin_error("Failed to decode Beast data");
        return NULL;
    }
    return result;
}

/* --- Beast2 encode/decode --- */

static EastValue *blob_encode_beast2(EastValue **args, size_t n) {
    (void)n;
    EastType *type = beast2_type_ctx;
    if (!type) {
        east_builtin_error("Beast2 encode: no type context");
        return NULL;
    }
    ByteBuffer *buf = east_beast2_encode_full(args[0], type);
    if (!buf) return east_blob(NULL, 0);
    EastValue *result = east_blob(buf->data, buf->len);
    byte_buffer_free(buf);
    return result;
}

static EastValue *blob_decode_beast2(EastValue **args, size_t n) {
    (void)n;
    EastType *type = beast2_type_ctx;
    if (!type) {
        east_builtin_error("Beast2 decode: no type context");
        return NULL;
    }
    EastValue *result = east_beast2_decode_full(
        args[0]->data.blob.data, args[0]->data.blob.len, type);
    if (!result) {
        east_builtin_error("Failed to decode Beast2 data");
        return NULL;
    }
    return result;
}

/* --- CSV decode --- */

static EastValue *blob_decode_csv(EastValue **args, size_t n) {
    EastType *struct_type = csv_struct_type_ctx;
    if (!struct_type) {
        east_builtin_error("CSV decode: no type context");
        return NULL;
    }

    /* Build the array type from the struct type */
    EastType *arr_type = east_array_type(struct_type);

    /* Convert blob to string */
    const char *csv_str = (const char *)args[0]->data.blob.data;
    size_t csv_len = args[0]->data.blob.len;

    /* Need null-terminated string */
    char *csv_copy = malloc(csv_len + 1);
    if (!csv_copy) {
        east_type_release(arr_type);
        return east_array_new(struct_type);
    }
    memcpy(csv_copy, csv_str, csv_len);
    csv_copy[csv_len] = '\0';

    /* args[0] = blob, args[1] = config (optional) */
    EastValue *config = (n > 1) ? args[1] : NULL;
    EastValue *result = east_csv_decode(csv_copy, arr_type, config);
    free(csv_copy);
    east_type_release(arr_type);

    if (!result) return east_array_new(struct_type);
    return result;
}

/* --- factory functions --- */

static BuiltinImpl blob_size_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return blob_size; }
static BuiltinImpl blob_get_uint8_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return blob_get_uint8; }
static BuiltinImpl blob_decode_utf8_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return blob_decode_utf8; }
static BuiltinImpl blob_decode_utf16_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return blob_decode_utf16; }

static BuiltinImpl blob_encode_beast_factory(EastType **tp, size_t ntp) {
    beast_type_ctx = (ntp > 0) ? tp[0] : NULL;
    return blob_encode_beast;
}
static BuiltinImpl blob_decode_beast_factory(EastType **tp, size_t ntp) {
    beast_type_ctx = (ntp > 0) ? tp[0] : NULL;
    return blob_decode_beast;
}
static BuiltinImpl blob_encode_beast2_factory(EastType **tp, size_t ntp) {
    beast2_type_ctx = (ntp > 0) ? tp[0] : NULL;
    return blob_encode_beast2;
}
static BuiltinImpl blob_decode_beast2_factory(EastType **tp, size_t ntp) {
    beast2_type_ctx = (ntp > 0) ? tp[0] : NULL;
    return blob_decode_beast2;
}
static BuiltinImpl blob_decode_csv_factory(EastType **tp, size_t ntp) {
    csv_struct_type_ctx = (ntp > 0) ? tp[0] : NULL;
    return blob_decode_csv;
}

static BuiltinImpl string_encode_utf8_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return string_encode_utf8; }
static BuiltinImpl string_encode_utf16_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return string_encode_utf16; }

/* --- registration --- */

void east_register_blob_builtins(BuiltinRegistry *reg) {
    builtin_registry_register(reg, "BlobSize", blob_size_factory);
    builtin_registry_register(reg, "BlobGetUint8", blob_get_uint8_factory);
    builtin_registry_register(reg, "BlobDecodeUtf8", blob_decode_utf8_factory);
    builtin_registry_register(reg, "BlobDecodeUtf16", blob_decode_utf16_factory);
    builtin_registry_register(reg, "BlobDecodeBeast", blob_decode_beast_factory);
    builtin_registry_register(reg, "BlobEncodeBeast", blob_encode_beast_factory);
    builtin_registry_register(reg, "BlobDecodeBeast2", blob_decode_beast2_factory);
    builtin_registry_register(reg, "BlobEncodeBeast2", blob_encode_beast2_factory);
    builtin_registry_register(reg, "BlobDecodeCsv", blob_decode_csv_factory);
    builtin_registry_register(reg, "StringEncodeUtf8", string_encode_utf8_factory);
    builtin_registry_register(reg, "StringEncodeUtf16", string_encode_utf16_factory);
}
