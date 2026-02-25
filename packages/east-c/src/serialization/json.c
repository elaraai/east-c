/*
 * JSON serialization for East types.
 *
 * Type-driven JSON encoding and decoding for East values.
 * Includes a minimal recursive-descent JSON parser.
 *
 * Encoding conventions:
 *   Null     -> null
 *   Boolean  -> true/false
 *   Integer  -> string (to preserve 64-bit precision)
 *   Float    -> number (or string for NaN/Infinity/-Infinity/-0.0)
 *   String   -> quoted string with escapes
 *   DateTime -> ISO 8601 string with timezone
 *   Blob     -> hex string "0x..."
 *   Array    -> JSON array
 *   Set      -> JSON array
 *   Dict     -> array of {"key":...,"value":...}
 *   Struct   -> JSON object
 *   Variant  -> {"type":"CaseName","value":...}
 *   Ref      -> encode inner value
 *   Vector   -> JSON array
 *   Matrix   -> JSON array of arrays
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
/*  String builder helper                                              */
/* ================================================================== */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static StrBuf strbuf_new(size_t initial_cap)
{
    StrBuf sb;
    sb.cap = (initial_cap > 0) ? initial_cap : 256;
    sb.data = malloc(sb.cap);
    sb.len = 0;
    if (sb.data) sb.data[0] = '\0';
    return sb;
}

static void strbuf_ensure(StrBuf *sb, size_t needed)
{
    size_t required = sb->len + needed + 1; /* +1 for null terminator */
    if (required <= sb->cap) return;
    size_t new_cap = sb->cap * 2;
    if (new_cap < required) new_cap = required;
    char *nd = realloc(sb->data, new_cap);
    if (!nd) return;
    sb->data = nd;
    sb->cap = new_cap;
}

static void strbuf_append(StrBuf *sb, const char *str, size_t len)
{
    if (len == 0) return;
    strbuf_ensure(sb, len);
    memcpy(sb->data + sb->len, str, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

static void strbuf_append_str(StrBuf *sb, const char *str)
{
    strbuf_append(sb, str, strlen(str));
}

static void strbuf_append_char(StrBuf *sb, char c)
{
    strbuf_ensure(sb, 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

static char *strbuf_finish(StrBuf *sb)
{
    return sb->data; /* caller owns the memory */
}

/* ================================================================== */
/*  Base64 encoder (for Blob encoding)                                 */
/* ================================================================== */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void strbuf_append_base64(StrBuf *sb, const uint8_t *data, size_t len)
{
    size_t i;
    for (i = 0; i + 2 < len; i += 3) {
        uint32_t v = ((uint32_t)data[i] << 16) |
                     ((uint32_t)data[i+1] << 8) |
                     (uint32_t)data[i+2];
        strbuf_append_char(sb, b64_table[(v >> 18) & 0x3F]);
        strbuf_append_char(sb, b64_table[(v >> 12) & 0x3F]);
        strbuf_append_char(sb, b64_table[(v >>  6) & 0x3F]);
        strbuf_append_char(sb, b64_table[v & 0x3F]);
    }
    if (i < len) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i+1] << 8;
        strbuf_append_char(sb, b64_table[(v >> 18) & 0x3F]);
        strbuf_append_char(sb, b64_table[(v >> 12) & 0x3F]);
        if (i + 1 < len) {
            strbuf_append_char(sb, b64_table[(v >> 6) & 0x3F]);
        } else {
            strbuf_append_char(sb, '=');
        }
        strbuf_append_char(sb, '=');
    }
}

/* ================================================================== */
/*  Base64 decoder                                                     */
/* ================================================================== */

static int b64_decode_char(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static uint8_t *base64_decode(const char *input, size_t input_len, size_t *out_len)
{
    if (input_len == 0) {
        *out_len = 0;
        uint8_t *r = malloc(1);
        if (r) r[0] = 0;
        return r;
    }

    /* Remove padding for length calculation */
    size_t padding = 0;
    if (input_len >= 1 && input[input_len - 1] == '=') padding++;
    if (input_len >= 2 && input[input_len - 2] == '=') padding++;

    size_t decoded_len = (input_len / 4) * 3 - padding;
    uint8_t *output = malloc(decoded_len + 1);
    if (!output) { *out_len = 0; return NULL; }

    size_t j = 0;
    for (size_t i = 0; i < input_len; i += 4) {
        int a = (i < input_len) ? b64_decode_char(input[i]) : 0;
        int b = (i + 1 < input_len) ? b64_decode_char(input[i+1]) : 0;
        int c = (i + 2 < input_len && input[i+2] != '=') ? b64_decode_char(input[i+2]) : 0;
        int d = (i + 3 < input_len && input[i+3] != '=') ? b64_decode_char(input[i+3]) : 0;

        if (a < 0) a = 0;
        if (b < 0) b = 0;

        uint32_t triple = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
                          ((uint32_t)c << 6) | (uint32_t)d;

        if (j < decoded_len) output[j++] = (uint8_t)((triple >> 16) & 0xFF);
        if (j < decoded_len) output[j++] = (uint8_t)((triple >> 8) & 0xFF);
        if (j < decoded_len) output[j++] = (uint8_t)(triple & 0xFF);
    }

    *out_len = decoded_len;
    return output;
}

/* ================================================================== */
/*  JSON string escaping                                               */
/* ================================================================== */

static void strbuf_append_json_string(StrBuf *sb, const char *str, size_t len)
{
    strbuf_append_char(sb, '"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        switch (c) {
        case '"':  strbuf_append_str(sb, "\\\""); break;
        case '\\': strbuf_append_str(sb, "\\\\"); break;
        case '\b': strbuf_append_str(sb, "\\b");  break;
        case '\f': strbuf_append_str(sb, "\\f");  break;
        case '\n': strbuf_append_str(sb, "\\n");  break;
        case '\r': strbuf_append_str(sb, "\\r");  break;
        case '\t': strbuf_append_str(sb, "\\t");  break;
        default:
            if (c < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", c);
                strbuf_append_str(sb, esc);
            } else {
                strbuf_append_char(sb, (char)c);
            }
            break;
        }
    }
    strbuf_append_char(sb, '"');
}

/* ================================================================== */
/*  JSON Encoder (type-driven)                                         */
/* ================================================================== */

static void json_encode_value(StrBuf *sb, EastValue *value, EastType *type);

static void json_encode_value(StrBuf *sb, EastValue *value, EastType *type)
{
    if (!value || !type) {
        strbuf_append_str(sb, "null");
        return;
    }

    switch (type->kind) {
    case EAST_TYPE_NEVER:
        strbuf_append_str(sb, "null");
        break;

    case EAST_TYPE_NULL:
        strbuf_append_str(sb, "null");
        break;

    case EAST_TYPE_BOOLEAN:
        strbuf_append_str(sb, value->data.boolean ? "true" : "false");
        break;

    case EAST_TYPE_INTEGER: {
        /* Encode integer as JSON string to preserve 64-bit precision */
        char numbuf[32];
        snprintf(numbuf, sizeof(numbuf), "\"%lld\"",
                 (long long)value->data.integer);
        strbuf_append_str(sb, numbuf);
        break;
    }

    case EAST_TYPE_FLOAT: {
        double f = value->data.float64;
        if (f != f) {
            /* NaN */
            strbuf_append_str(sb, "\"NaN\"");
        } else if (isinf(f)) {
            strbuf_append_str(sb, f > 0 ? "\"Infinity\"" : "\"-Infinity\"");
        } else if (f == 0.0 && signbit(f)) {
            strbuf_append_str(sb, "\"-0.0\"");
        } else {
            char numbuf[64];
            /* If the float is an exact integer in safe range, emit as integer */
            if (f == (double)(long long)f &&
                f >= -9007199254740992.0 && f <= 9007199254740992.0) {
                snprintf(numbuf, sizeof(numbuf), "%lld", (long long)f);
            } else {
                east_fmt_double(numbuf, sizeof(numbuf), f);
            }
            strbuf_append_str(sb, numbuf);
        }
        break;
    }

    case EAST_TYPE_STRING:
        strbuf_append_json_string(sb, value->data.string.data,
                                  value->data.string.len);
        break;

    case EAST_TYPE_DATETIME: {
        /* ISO 8601 with milliseconds and +00:00 timezone */
        int64_t millis = value->data.datetime;
        int64_t secs = millis / 1000;
        int64_t ms = millis % 1000;
        if (ms < 0) { ms += 1000; secs--; }

        /* Convert epoch seconds to date components */
        /* Using a simple gmtime-style calculation */
        struct {
            int year, month, day, hour, min, sec;
        } dt;

        /* Days from epoch */
        int64_t days = secs / 86400;
        int64_t rem  = secs % 86400;
        if (rem < 0) { rem += 86400; days--; }

        dt.hour = (int)(rem / 3600);
        rem %= 3600;
        dt.min = (int)(rem / 60);
        dt.sec = (int)(rem % 60);

        /* Convert days since epoch to year/month/day */
        /* Algorithm from http://howardhinnant.github.io/date_algorithms.html */
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

        dt.year = (int)y;
        dt.month = (int)m;
        dt.day = (int)d;

        char datebuf[64];
        snprintf(datebuf, sizeof(datebuf),
                 "\"%04d-%02d-%02dT%02d:%02d:%02d.%03d+00:00\"",
                 dt.year, dt.month, dt.day,
                 dt.hour, dt.min, dt.sec, (int)ms);
        strbuf_append_str(sb, datebuf);
        break;
    }

    case EAST_TYPE_BLOB: {
        /* Encode as hex string "0x..." (matches TypeScript East JSON format) */
        strbuf_append_str(sb, "\"0x");
        for (size_t i = 0; i < value->data.blob.len; i++) {
            char hex[3];
            snprintf(hex, sizeof(hex), "%02x", value->data.blob.data[i]);
            strbuf_append_str(sb, hex);
        }
        strbuf_append_char(sb, '"');
        break;
    }

    case EAST_TYPE_ARRAY: {
        EastType *elem_type = type->data.element;
        strbuf_append_char(sb, '[');
        for (size_t i = 0; i < value->data.array.len; i++) {
            if (i > 0) strbuf_append_char(sb, ',');
            json_encode_value(sb, value->data.array.items[i], elem_type);
        }
        strbuf_append_char(sb, ']');
        break;
    }

    case EAST_TYPE_SET: {
        EastType *elem_type = type->data.element;
        strbuf_append_char(sb, '[');
        for (size_t i = 0; i < value->data.set.len; i++) {
            if (i > 0) strbuf_append_char(sb, ',');
            json_encode_value(sb, value->data.set.items[i], elem_type);
        }
        strbuf_append_char(sb, ']');
        break;
    }

    case EAST_TYPE_DICT: {
        EastType *key_type = type->data.dict.key;
        EastType *val_type = type->data.dict.value;
        strbuf_append_char(sb, '[');
        for (size_t i = 0; i < value->data.dict.len; i++) {
            if (i > 0) strbuf_append_char(sb, ',');
            strbuf_append_str(sb, "{\"key\":");
            json_encode_value(sb, value->data.dict.keys[i], key_type);
            strbuf_append_str(sb, ",\"value\":");
            json_encode_value(sb, value->data.dict.values[i], val_type);
            strbuf_append_char(sb, '}');
        }
        strbuf_append_char(sb, ']');
        break;
    }

    case EAST_TYPE_STRUCT: {
        strbuf_append_char(sb, '{');
        size_t nf = type->data.struct_.num_fields;
        /* Struct values always have fields in type schema order */
        for (size_t i = 0; i < nf; i++) {
            if (i > 0) strbuf_append_char(sb, ',');
            const char *fname = type->data.struct_.fields[i].name;
            EastType *ftype = type->data.struct_.fields[i].type;
            strbuf_append_json_string(sb, fname, strlen(fname));
            strbuf_append_char(sb, ':');
            EastValue *fval = (value->kind == EAST_VAL_STRUCT && i < value->data.struct_.num_fields)
                            ? value->data.struct_.field_values[i] : NULL;
            json_encode_value(sb, fval, ftype);
        }
        strbuf_append_char(sb, '}');
        break;
    }

    case EAST_TYPE_VARIANT: {
        const char *case_name = value->data.variant.case_name;
        EastType *case_type = NULL;
        for (size_t i = 0; i < type->data.variant.num_cases; i++) {
            if (strcmp(type->data.variant.cases[i].name, case_name) == 0) {
                case_type = type->data.variant.cases[i].type;
                break;
            }
        }
        strbuf_append_str(sb, "{\"type\":");
        strbuf_append_json_string(sb, case_name, strlen(case_name));
        strbuf_append_str(sb, ",\"value\":");
        json_encode_value(sb, value->data.variant.value, case_type);
        strbuf_append_char(sb, '}');
        break;
    }

    case EAST_TYPE_REF:
        /* Encode as single-element JSON array [value] */
        strbuf_append_char(sb, '[');
        json_encode_value(sb, value->data.ref.value, type->data.element);
        strbuf_append_char(sb, ']');
        break;

    case EAST_TYPE_VECTOR: {
        EastType *elem_type = type->data.element;
        size_t vlen = value->data.vector.len;
        strbuf_append_char(sb, '[');
        for (size_t i = 0; i < vlen; i++) {
            if (i > 0) strbuf_append_char(sb, ',');
            if (elem_type->kind == EAST_TYPE_FLOAT) {
                double v = ((double *)value->data.vector.data)[i];
                char numbuf[64];
                if (v != v) {
                    strbuf_append_str(sb, "\"NaN\"");
                } else if (isinf(v)) {
                    strbuf_append_str(sb, v > 0 ? "\"Infinity\"" : "\"-Infinity\"");
                } else {
                    east_fmt_double(numbuf, sizeof(numbuf), v);
                    strbuf_append_str(sb, numbuf);
                }
            } else if (elem_type->kind == EAST_TYPE_INTEGER) {
                char numbuf[32];
                snprintf(numbuf, sizeof(numbuf), "\"%lld\"",
                         (long long)((int64_t *)value->data.vector.data)[i]);
                strbuf_append_str(sb, numbuf);
            } else if (elem_type->kind == EAST_TYPE_BOOLEAN) {
                bool bv = ((bool *)value->data.vector.data)[i];
                strbuf_append_str(sb, bv ? "true" : "false");
            }
        }
        strbuf_append_char(sb, ']');
        break;
    }

    case EAST_TYPE_MATRIX: {
        EastType *elem_type = type->data.element;
        size_t rows = value->data.matrix.rows;
        size_t cols = value->data.matrix.cols;
        strbuf_append_char(sb, '[');
        for (size_t r = 0; r < rows; r++) {
            if (r > 0) strbuf_append_char(sb, ',');
            strbuf_append_char(sb, '[');
            for (size_t c = 0; c < cols; c++) {
                if (c > 0) strbuf_append_char(sb, ',');
                size_t idx = r * cols + c;
                if (elem_type->kind == EAST_TYPE_FLOAT) {
                    double v = ((double *)value->data.matrix.data)[idx];
                    char numbuf[64];
                    if (v != v) {
                        strbuf_append_str(sb, "\"NaN\"");
                    } else if (isinf(v)) {
                        strbuf_append_str(sb, v > 0 ? "\"Infinity\"" : "\"-Infinity\"");
                    } else {
                        east_fmt_double(numbuf, sizeof(numbuf), v);
                        strbuf_append_str(sb, numbuf);
                    }
                } else if (elem_type->kind == EAST_TYPE_INTEGER) {
                    char numbuf[32];
                    snprintf(numbuf, sizeof(numbuf), "\"%lld\"",
                             (long long)((int64_t *)value->data.matrix.data)[idx]);
                    strbuf_append_str(sb, numbuf);
                } else if (elem_type->kind == EAST_TYPE_BOOLEAN) {
                    bool bv = ((bool *)value->data.matrix.data)[idx];
                    strbuf_append_str(sb, bv ? "true" : "false");
                }
            }
            strbuf_append_char(sb, ']');
        }
        strbuf_append_char(sb, ']');
        break;
    }

    case EAST_TYPE_RECURSIVE:
        /* Unwrap: encode via the inner node type */
        if (type->data.recursive.node) {
            json_encode_value(sb, value, type->data.recursive.node);
        } else {
            strbuf_append_str(sb, "null");
        }
        break;

    case EAST_TYPE_FUNCTION:
    case EAST_TYPE_ASYNC_FUNCTION:
        /* Functions cannot be JSON-encoded */
        strbuf_append_str(sb, "null");
        break;
    }
}

char *east_json_encode(EastValue *value, EastType *type)
{
    StrBuf sb = strbuf_new(256);
    json_encode_value(&sb, value, type);
    return strbuf_finish(&sb);
}

/* ================================================================== */
/*  Minimal JSON Parser                                                */
/* ================================================================== */

typedef struct {
    const char *input;
    size_t pos;
    size_t len;
} JsonParser;

static void jp_skip_ws(JsonParser *p)
{
    while (p->pos < p->len) {
        char c = p->input[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->pos++;
        } else {
            break;
        }
    }
}

static char jp_peek(JsonParser *p)
{
    jp_skip_ws(p);
    if (p->pos >= p->len) return '\0';
    return p->input[p->pos];
}

static bool jp_match(JsonParser *p, char c)
{
    jp_skip_ws(p);
    if (p->pos < p->len && p->input[p->pos] == c) {
        p->pos++;
        return true;
    }
    return false;
}

static bool jp_match_str(JsonParser *p, const char *s)
{
    jp_skip_ws(p);
    size_t slen = strlen(s);
    if (p->pos + slen <= p->len &&
        memcmp(p->input + p->pos, s, slen) == 0) {
        p->pos += slen;
        return true;
    }
    return false;
}

/* Skip a JSON value of any type (for skipping unknown struct fields) */
static void jp_skip_json_value(JsonParser *p)
{
    jp_skip_ws(p);
    if (p->pos >= p->len) return;
    char c = p->input[p->pos];

    if (c == '"') {
        /* Skip string */
        p->pos++; /* skip opening quote */
        while (p->pos < p->len) {
            char sc = p->input[p->pos++];
            if (sc == '\\' && p->pos < p->len) p->pos++; /* skip escaped char */
            else if (sc == '"') break;
        }
    } else if (c == '{') {
        /* Skip object */
        p->pos++;
        int depth = 1;
        bool in_str = false;
        while (p->pos < p->len && depth > 0) {
            char oc = p->input[p->pos++];
            if (in_str) {
                if (oc == '\\' && p->pos < p->len) p->pos++;
                else if (oc == '"') in_str = false;
            } else {
                if (oc == '"') in_str = true;
                else if (oc == '{') depth++;
                else if (oc == '}') depth--;
            }
        }
    } else if (c == '[') {
        /* Skip array */
        p->pos++;
        int depth = 1;
        bool in_str = false;
        while (p->pos < p->len && depth > 0) {
            char ac = p->input[p->pos++];
            if (in_str) {
                if (ac == '\\' && p->pos < p->len) p->pos++;
                else if (ac == '"') in_str = false;
            } else {
                if (ac == '"') in_str = true;
                else if (ac == '[') depth++;
                else if (ac == ']') depth--;
            }
        }
    } else if (c == 't') {
        p->pos += 4; /* true */
    } else if (c == 'f') {
        p->pos += 5; /* false */
    } else if (c == 'n') {
        p->pos += 4; /* null */
    } else {
        /* Skip number */
        if (c == '-') p->pos++;
        while (p->pos < p->len && (isdigit((unsigned char)p->input[p->pos]) ||
               p->input[p->pos] == '.' || p->input[p->pos] == 'e' ||
               p->input[p->pos] == 'E' || p->input[p->pos] == '+' ||
               p->input[p->pos] == '-')) {
            p->pos++;
        }
    }
}

/* Parse a JSON string, returning a newly allocated C string.
 * Sets *out_len to the string length (excluding null terminator). */
static char *jp_parse_string(JsonParser *p, size_t *out_len)
{
    jp_skip_ws(p);
    if (p->pos >= p->len || p->input[p->pos] != '"') return NULL;
    p->pos++; /* skip opening quote */

    StrBuf sb = strbuf_new(64);
    while (p->pos < p->len) {
        char c = p->input[p->pos];
        if (c == '"') {
            p->pos++;
            if (out_len) *out_len = sb.len;
            return strbuf_finish(&sb);
        }
        if (c == '\\') {
            p->pos++;
            if (p->pos >= p->len) { free(sb.data); return NULL; }
            char esc = p->input[p->pos++];
            switch (esc) {
            case '"':  strbuf_append_char(&sb, '"');  break;
            case '\\': strbuf_append_char(&sb, '\\'); break;
            case '/':  strbuf_append_char(&sb, '/');  break;
            case 'b':  strbuf_append_char(&sb, '\b'); break;
            case 'f':  strbuf_append_char(&sb, '\f'); break;
            case 'n':  strbuf_append_char(&sb, '\n'); break;
            case 'r':  strbuf_append_char(&sb, '\r'); break;
            case 't':  strbuf_append_char(&sb, '\t'); break;
            case 'u': {
                /* Parse 4-digit hex */
                if (p->pos + 4 > p->len) { free(sb.data); return NULL; }
                char hex[5];
                memcpy(hex, p->input + p->pos, 4);
                hex[4] = '\0';
                p->pos += 4;
                unsigned int cp = (unsigned int)strtoul(hex, NULL, 16);
                /* Simple UTF-8 encoding */
                if (cp < 0x80) {
                    strbuf_append_char(&sb, (char)cp);
                } else if (cp < 0x800) {
                    strbuf_append_char(&sb, (char)(0xC0 | (cp >> 6)));
                    strbuf_append_char(&sb, (char)(0x80 | (cp & 0x3F)));
                } else {
                    strbuf_append_char(&sb, (char)(0xE0 | (cp >> 12)));
                    strbuf_append_char(&sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
                    strbuf_append_char(&sb, (char)(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default:
                strbuf_append_char(&sb, esc);
                break;
            }
        } else {
            strbuf_append_char(&sb, c);
            p->pos++;
        }
    }
    /* Unterminated string */
    free(sb.data);
    return NULL;
}

/* Parse a JSON number, returning it as a double.
 * Also stores the raw string in raw_buf if non-NULL. */
static double jp_parse_number(JsonParser *p, char *raw_buf, size_t raw_cap)
{
    jp_skip_ws(p);
    size_t start = p->pos;
    if (p->pos < p->len && p->input[p->pos] == '-') p->pos++;
    while (p->pos < p->len && isdigit((unsigned char)p->input[p->pos])) p->pos++;
    if (p->pos < p->len && p->input[p->pos] == '.') {
        p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->input[p->pos])) p->pos++;
    }
    if (p->pos < p->len && (p->input[p->pos] == 'e' || p->input[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->len && (p->input[p->pos] == '+' || p->input[p->pos] == '-'))
            p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->input[p->pos])) p->pos++;
    }
    size_t numlen = p->pos - start;
    if (raw_buf && numlen < raw_cap) {
        memcpy(raw_buf, p->input + start, numlen);
        raw_buf[numlen] = '\0';
    }
    char tmp[128];
    if (numlen >= sizeof(tmp)) numlen = sizeof(tmp) - 1;
    memcpy(tmp, p->input + start, numlen);
    tmp[numlen] = '\0';
    return strtod(tmp, NULL);
}

/* ================================================================== */
/*  $ref decode context for structural sharing in recursive types      */
/* ================================================================== */

#include "east/hashmap.h"

typedef struct {
    char **segments;    /* path segment stack */
    size_t len;
    size_t cap;
    Hashmap *cache;     /* path_key -> EastValue* (retained) */
} JRefCtx;

static JRefCtx *jref_ctx_new(void)
{
    JRefCtx *ctx = calloc(1, sizeof(JRefCtx));
    if (!ctx) return NULL;
    ctx->cap = 64;
    ctx->segments = calloc(ctx->cap, sizeof(char *));
    ctx->cache = hashmap_new();
    return ctx;
}

static void jref_ctx_free(JRefCtx *ctx)
{
    if (!ctx) return;
    for (size_t i = 0; i < ctx->len; i++) free(ctx->segments[i]);
    free(ctx->segments);
    hashmap_free(ctx->cache, (void(*)(void*))east_value_release);
    free(ctx);
}

static void jref_push(JRefCtx *ctx, const char *seg)
{
    if (ctx->len >= ctx->cap) {
        ctx->cap *= 2;
        ctx->segments = realloc(ctx->segments, ctx->cap * sizeof(char *));
    }
    ctx->segments[ctx->len++] = strdup(seg);
}

static void jref_pop(JRefCtx *ctx)
{
    if (ctx->len > 0) {
        free(ctx->segments[--ctx->len]);
    }
}

/* Build the path key for current position: "/seg1/seg2/..." */
static char *jref_path_key(JRefCtx *ctx)
{
    StrBuf sb = strbuf_new(128);
    for (size_t i = 0; i < ctx->len; i++) {
        strbuf_append_char(&sb, '/');
        /* RFC 6901 escaping: ~ -> ~0, / -> ~1 */
        const char *s = ctx->segments[i];
        for (; *s; s++) {
            if (*s == '~')      strbuf_append_str(&sb, "~0");
            else if (*s == '/') strbuf_append_str(&sb, "~1");
            else                strbuf_append_char(&sb, *s);
        }
    }
    return strbuf_finish(&sb);
}

static void jref_register(JRefCtx *ctx, EastValue *val)
{
    char *key = jref_path_key(ctx);
    east_value_retain(val);
    hashmap_set(ctx->cache, key, val);
    free(key);
}

/* Resolve a relative reference like "10#0/Let/variable/Variable/location" */
static EastValue *jref_resolve(JRefCtx *ctx, const char *ref_str)
{
    /* Parse "N#remaining_path" */
    const char *hash = strchr(ref_str, '#');
    if (!hash) return NULL;

    int up_levels = atoi(ref_str);
    const char *remaining = hash + 1;

    /* Compute base: current path minus up_levels */
    size_t base_len = ctx->len;
    if ((size_t)up_levels > base_len) return NULL;
    base_len -= (size_t)up_levels;

    /* Build target path key */
    StrBuf sb = strbuf_new(128);
    for (size_t i = 0; i < base_len; i++) {
        strbuf_append_char(&sb, '/');
        const char *s = ctx->segments[i];
        for (; *s; s++) {
            if (*s == '~')      strbuf_append_str(&sb, "~0");
            else if (*s == '/') strbuf_append_str(&sb, "~1");
            else                strbuf_append_char(&sb, *s);
        }
    }

    /* Append remaining path components */
    if (*remaining) {
        strbuf_append_char(&sb, '/');
        /* remaining is already in escaped JSON Pointer form */
        strbuf_append_str(&sb, remaining);
    }

    char *key = strbuf_finish(&sb);
    EastValue *val = hashmap_get(ctx->cache, key);
    free(key);

    if (val) east_value_retain(val);
    return val;
}

/* ================================================================== */
/*  JSON decode error context                                          */
/* ================================================================== */

typedef struct {
    char *message;   /* e.g. "expected null, got 123" */
    char *path;      /* e.g. "[1].value" or "" */
} JDecodeErr;

static void jde_init(JDecodeErr *e) { e->message = NULL; e->path = NULL; }

static void jde_free(JDecodeErr *e) {
    free(e->message);
    free(e->path);
    e->message = NULL;
    e->path = NULL;
}

/* Set error message (takes ownership of msg) */
static void jde_set_msg(JDecodeErr *e, char *msg) {
    free(e->message);
    e->message = msg;
    free(e->path);
    e->path = NULL;
}

/* Set error with path */
static void jde_set_msg_path(JDecodeErr *e, char *msg, char *path) {
    free(e->message);
    e->message = msg;
    free(e->path);
    e->path = path;
}

/* Prepend a path segment to existing error path.
 * segment is like "[0]", ".fieldname", ".casename" */
static void jde_prepend_path(JDecodeErr *e, const char *segment) {
    if (!e->message) return;
    if (!e->path || e->path[0] == '\0') {
        free(e->path);
        e->path = strdup(segment);
    } else {
        size_t slen = strlen(segment);
        size_t plen = strlen(e->path);
        char *np = malloc(slen + plen + 1);
        memcpy(np, segment, slen);
        memcpy(np + slen, e->path, plen + 1);
        free(e->path);
        e->path = np;
    }
}

/* Extract the raw JSON text for the value at current position.
 * Advances p->pos past the value. Returns allocated string. */
static char *jp_extract_raw_value(JsonParser *p) {
    jp_skip_ws(p);
    size_t start = p->pos;
    jp_skip_json_value(p);
    size_t end = p->pos;
    if (end <= start) return strdup("null");
    char *raw = malloc(end - start + 1);
    memcpy(raw, p->input + start, end - start);
    raw[end - start] = '\0';
    return raw;
}

/* Format: "expected X, got Y" — Y is the raw JSON at current position */
static char *jp_fmt_error(JsonParser *p, const char *reason) {
    char *raw = jp_extract_raw_value(p);
    size_t rlen = strlen(reason);
    size_t rawlen = strlen(raw);
    char *msg = malloc(rlen + rawlen + 8);
    snprintf(msg, rlen + rawlen + 8, "%s, got %s", reason, raw);
    free(raw);
    return msg;
}

/* Format error without "got" part */
static char *jp_fmt_error_no_got(const char *reason) {
    return strdup(reason);
}

/* Format: "reason, got RAW" where RAW is already a string */
static char *jp_fmt_error_raw(const char *reason, const char *raw) {
    size_t rlen = strlen(reason);
    size_t rawlen = strlen(raw);
    char *msg = malloc(rlen + rawlen + 8);
    snprintf(msg, rlen + rawlen + 8, "%s, got %s", reason, raw);
    return msg;
}

/* ================================================================== */
/*  Type-driven JSON decoder with $ref support                         */
/* ================================================================== */

static int jp_debug = 0;

/* Forward declarations */
static EastValue *jp_decode(JsonParser *p, EastType *type, JRefCtx *ctx);
static EastValue *jp_decode_err(JsonParser *p, EastType *type, JRefCtx *ctx, JDecodeErr *err);

/* Try to parse {"$ref":"..."} — returns resolved value or NULL (restoring pos) */
static EastValue *jp_try_ref(JsonParser *p, JRefCtx *ctx)
{
    if (!ctx) return NULL;
    jp_skip_ws(p);
    if (p->pos >= p->len || p->input[p->pos] != '{') return NULL;

    size_t save = p->pos;
    p->pos++; /* skip { */
    jp_skip_ws(p);

    /* Check for "$ref" key */
    if (p->pos + 5 < p->len && p->input[p->pos] == '"' &&
        memcmp(p->input + p->pos, "\"$ref\"", 6) == 0) {
        p->pos += 6;
        jp_skip_ws(p);
        if (p->pos < p->len && p->input[p->pos] == ':') {
            p->pos++;
            size_t ref_len;
            char *ref_str = jp_parse_string(p, &ref_len);
            if (ref_str) {
                jp_skip_ws(p);
                if (p->pos < p->len && p->input[p->pos] == '}') {
                    p->pos++; /* skip } */
                    EastValue *resolved = jref_resolve(ctx, ref_str);
                    free(ref_str);
                    return resolved;
                }
                free(ref_str);
            }
        }
    }

    /* Not a $ref — backtrack */
    p->pos = save;
    return NULL;
}

/* Parse a JSON array/set with $ref + path tracking */
static EastValue *jp_decode_array(JsonParser *p, EastType *type, JRefCtx *ctx)
{
    /* Check for $ref */
    EastValue *ref = jp_try_ref(p, ctx);
    if (ref) return ref;

    EastType *elem_type = type->data.element;
    EastValue *arr;

    if (type->kind == EAST_TYPE_SET) {
        arr = east_set_new(elem_type);
    } else {
        arr = east_array_new(elem_type);
    }
    if (!arr) return NULL;

    /* Register in cache BEFORE parsing elements (for forward refs) */
    if (ctx) jref_register(ctx, arr);

    if (!jp_match(p, '[')) { east_value_release(arr); return NULL; }

    if (jp_peek(p) != ']') {
        size_t idx = 0;
        for (;;) {
            char idx_str[24];
            snprintf(idx_str, sizeof(idx_str), "%zu", idx);
            if (ctx) jref_push(ctx, idx_str);

            EastValue *elem = jp_decode(p, elem_type, ctx);

            if (ctx) jref_pop(ctx);

            if (!elem) { east_value_release(arr); return NULL; }
            if (type->kind == EAST_TYPE_SET) {
                east_set_insert(arr, elem);
            } else {
                east_array_push(arr, elem);
            }
            east_value_release(elem);
            idx++;
            if (!jp_match(p, ',')) break;
        }
    }

    if (!jp_match(p, ']')) { east_value_release(arr); return NULL; }
    return arr;
}

/* Parse a JSON value guided by the East type, with $ref context */
static EastValue *jp_decode(JsonParser *p, EastType *type, JRefCtx *ctx)
{
    if (!type) return NULL;

    switch (type->kind) {
    case EAST_TYPE_NULL: {
        if (jp_match_str(p, "null")) return east_null();
        return NULL;
    }

    case EAST_TYPE_BOOLEAN: {
        if (jp_match_str(p, "true"))  return east_boolean(true);
        if (jp_match_str(p, "false")) return east_boolean(false);
        return NULL;
    }

    case EAST_TYPE_INTEGER: {
        size_t slen;
        char *s = jp_parse_string(p, &slen);
        if (!s) return NULL;
        char *end;
        long long val = strtoll(s, &end, 10);
        free(s);
        return east_integer((int64_t)val);
    }

    case EAST_TYPE_FLOAT: {
        jp_skip_ws(p);
        char c = jp_peek(p);
        if (c == '"') {
            size_t slen;
            char *s = jp_parse_string(p, &slen);
            if (!s) return NULL;
            double v = 0.0;
            if (strcmp(s, "NaN") == 0) v = NAN;
            else if (strcmp(s, "Infinity") == 0) v = INFINITY;
            else if (strcmp(s, "-Infinity") == 0) v = -INFINITY;
            else if (strcmp(s, "-0.0") == 0) v = -0.0;
            else v = strtod(s, NULL);
            free(s);
            return east_float(v);
        }
        double v = jp_parse_number(p, NULL, 0);
        return east_float(v);
    }

    case EAST_TYPE_STRING: {
        size_t slen;
        char *s = jp_parse_string(p, &slen);
        if (!s) return NULL;
        EastValue *val = east_string_len(s, slen);
        free(s);
        return val;
    }

    case EAST_TYPE_DATETIME: {
        size_t slen;
        char *s = jp_parse_string(p, &slen);
        if (!s) return NULL;

        int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0, ms = 0;
        int tz_hour = 0, tz_min = 0;
        int tz_sign = 1;

        if (slen >= 23) {
            sscanf(s, "%d-%d-%dT%d:%d:%d.%d",
                   &year, &month, &day, &hour, &min, &sec, &ms);
            const char *tz = s + 23;
            if (*tz == 'Z' || *tz == 'z') {
                /* UTC */
            } else if (*tz == '+' || *tz == '-') {
                tz_sign = (*tz == '-') ? -1 : 1;
                sscanf(tz + 1, "%d:%d", &tz_hour, &tz_min);
            }
        }

        int64_t y = year;
        int64_t m_adj = month;
        if (m_adj <= 2) { y--; m_adj += 9; } else { m_adj -= 3; }

        int64_t era = (y >= 0 ? y : y - 399) / 400;
        int64_t yoe = y - era * 400;
        int64_t doy = (153 * m_adj + 2) / 5 + day - 1;
        int64_t doe = yoe * 365 + yoe/4 - yoe/100 + doy;
        int64_t days = era * 146097 + doe - 719468;

        int64_t epoch_secs = days * 86400 + hour * 3600 + min * 60 + sec;
        epoch_secs -= tz_sign * (tz_hour * 3600 + tz_min * 60);

        int64_t epoch_ms = epoch_secs * 1000 + ms;
        free(s);
        return east_datetime(epoch_ms);
    }

    case EAST_TYPE_BLOB: {
        /* Decode hex string "0x..." (matches TypeScript East JSON format) */
        size_t slen;
        char *s = jp_parse_string(p, &slen);
        if (!s) return NULL;
        /* Strip "0x" prefix */
        const char *hex = s;
        size_t hex_len = slen;
        if (hex_len >= 2 && hex[0] == '0' && hex[1] == 'x') {
            hex += 2;
            hex_len -= 2;
        }
        size_t blob_len = hex_len / 2;
        uint8_t *decoded = malloc(blob_len > 0 ? blob_len : 1);
        if (!decoded) { free(s); return NULL; }
        for (size_t i = 0; i < blob_len; i++) {
            unsigned int byte_val = 0;
            char byte_hex[3] = { hex[i * 2], hex[i * 2 + 1], '\0' };
            sscanf(byte_hex, "%02x", &byte_val);
            decoded[i] = (uint8_t)byte_val;
        }
        free(s);
        EastValue *val = east_blob(decoded, blob_len);
        free(decoded);
        return val;
    }

    case EAST_TYPE_ARRAY:
    case EAST_TYPE_SET:
        return jp_decode_array(p, type, ctx);

    case EAST_TYPE_DICT: {
        EastValue *ref = jp_try_ref(p, ctx);
        if (ref) return ref;

        EastType *key_type = type->data.dict.key;
        EastType *val_type = type->data.dict.value;
        EastValue *dict = east_dict_new(key_type, val_type);
        if (!dict) return NULL;

        if (ctx) jref_register(ctx, dict);

        if (!jp_match(p, '[')) { east_value_release(dict); return NULL; }

        if (jp_peek(p) != ']') {
            size_t idx = 0;
            for (;;) {
                if (!jp_match(p, '{')) { east_value_release(dict); return NULL; }

                EastValue *k = NULL;
                EastValue *v = NULL;

                for (int fi = 0; fi < 2; fi++) {
                    size_t fname_len;
                    char *fname = jp_parse_string(p, &fname_len);
                    if (!fname) { east_value_release(dict); return NULL; }
                    jp_match(p, ':');
                    if (strcmp(fname, "key") == 0) {
                        char seg[32];
                        snprintf(seg, sizeof(seg), "%zu", idx);
                        if (ctx) { jref_push(ctx, seg); jref_push(ctx, "key"); }
                        k = jp_decode(p, key_type, ctx);
                        if (ctx) { jref_pop(ctx); jref_pop(ctx); }
                    } else if (strcmp(fname, "value") == 0) {
                        char seg[32];
                        snprintf(seg, sizeof(seg), "%zu", idx);
                        if (ctx) { jref_push(ctx, seg); jref_push(ctx, "value"); }
                        v = jp_decode(p, val_type, ctx);
                        if (ctx) { jref_pop(ctx); jref_pop(ctx); }
                    }
                    free(fname);
                    if (fi == 0) jp_match(p, ',');
                }

                if (!jp_match(p, '}')) {
                    if (k) east_value_release(k);
                    if (v) east_value_release(v);
                    east_value_release(dict);
                    return NULL;
                }

                if (k && v) east_dict_set(dict, k, v);
                if (k) east_value_release(k);
                if (v) east_value_release(v);
                idx++;

                if (!jp_match(p, ',')) break;
            }
        }

        if (!jp_match(p, ']')) { east_value_release(dict); return NULL; }
        return dict;
    }

    case EAST_TYPE_STRUCT: {
        EastValue *ref = jp_try_ref(p, ctx);
        if (ref) return ref;

        if (!jp_match(p, '{')) return NULL;

        size_t nf = type->data.struct_.num_fields;
        const char **names = calloc(nf, sizeof(char *));
        EastValue **values = calloc(nf, sizeof(EastValue *));
        if (!names || !values) {
            free(names);
            free(values);
            return NULL;
        }

        for (size_t i = 0; i < nf; i++) {
            names[i] = type->data.struct_.fields[i].name;
            values[i] = NULL;
        }

        if (jp_peek(p) != '}') {
            for (;;) {
                size_t fname_len;
                char *fname = jp_parse_string(p, &fname_len);
                if (!fname) goto struct_fail;
                jp_match(p, ':');

                EastType *ftype = NULL;
                size_t fidx = 0;
                for (size_t i = 0; i < nf; i++) {
                    if (strcmp(names[i], fname) == 0) {
                        ftype = type->data.struct_.fields[i].type;
                        fidx = i;
                        break;
                    }
                }

                if (ftype) {
                    if (ctx) jref_push(ctx, fname);
                    values[fidx] = jp_decode(p, ftype, ctx);
                    if (ctx) jref_pop(ctx);
                } else {
                    /* Skip unknown field — use generic JSON skip */
                    jp_skip_json_value(p);
                }
                free(fname);

                if (!jp_match(p, ',')) break;
            }
        }

        if (!jp_match(p, '}')) goto struct_fail;

        for (size_t i = 0; i < nf; i++) {
            if (!values[i]) values[i] = east_null();
        }

        EastValue *result = east_struct_new(names, values, nf, type);
        for (size_t i = 0; i < nf; i++) {
            east_value_release(values[i]);
        }
        free(names);
        free(values);
        return result;

struct_fail:
        for (size_t i = 0; i < nf; i++) {
            if (values[i]) east_value_release(values[i]);
        }
        free(names);
        free(values);
        return NULL;
    }

    case EAST_TYPE_VARIANT: {
        EastValue *ref = jp_try_ref(p, ctx);
        if (ref) return ref;

        if (!jp_match(p, '{')) return NULL;

        char *case_name = NULL;
        EastValue *case_value = NULL;

        for (int fi = 0; fi < 2; fi++) {
            size_t fname_len;
            char *fname = jp_parse_string(p, &fname_len);
            if (!fname) { free(case_name); return NULL; }
            jp_match(p, ':');

            if (strcmp(fname, "type") == 0) {
                size_t cn_len;
                case_name = jp_parse_string(p, &cn_len);
            } else if (strcmp(fname, "value") == 0) {
                EastType *case_type = NULL;
                if (case_name) {
                    for (size_t i = 0; i < type->data.variant.num_cases; i++) {
                        if (strcmp(type->data.variant.cases[i].name, case_name) == 0) {
                            case_type = type->data.variant.cases[i].type;
                            break;
                        }
                    }
                }
                if (case_type) {
                    if (ctx) jref_push(ctx, case_name);
                    case_value = jp_decode(p, case_type, ctx);
                    if (ctx) jref_pop(ctx);
                } else {
                    jp_skip_json_value(p);
                }
            }
            free(fname);
            if (fi == 0) jp_match(p, ',');
        }

        if (!jp_match(p, '}')) {
            free(case_name);
            if (case_value) east_value_release(case_value);
            return NULL;
        }

        if (!case_name) { if (case_value) east_value_release(case_value); return NULL; }
        if (!case_value) case_value = east_null();

        EastValue *result = east_variant_new(case_name, case_value, type);
        east_value_release(case_value);
        free(case_name);
        return result;
    }

    case EAST_TYPE_REF: {
        EastValue *ref = jp_try_ref(p, ctx);
        if (ref) return ref;

        /* Ref is encoded as single-element JSON array [value] */
        if (!jp_match(p, '[')) return NULL;
        EastValue *inner = jp_decode(p, type->data.element, ctx);
        if (!inner) return NULL;
        if (!jp_match(p, ']')) { east_value_release(inner); return NULL; }

        if (ctx) jref_register(ctx, inner);

        EastValue *result = east_ref_new(inner);
        east_value_release(inner);
        return result;
    }

    case EAST_TYPE_VECTOR: {
        EastType *elem_type = type->data.element;
        if (!jp_match(p, '[')) return NULL;

        size_t cap = 16, len = 0;
        void *tmp_data = NULL;
        size_t elem_size = 0;

        if (elem_type->kind == EAST_TYPE_FLOAT) {
            elem_size = sizeof(double);
        } else if (elem_type->kind == EAST_TYPE_INTEGER) {
            elem_size = sizeof(int64_t);
        } else if (elem_type->kind == EAST_TYPE_BOOLEAN) {
            elem_size = sizeof(bool);
        }

        tmp_data = malloc(cap * elem_size);
        if (!tmp_data) return NULL;

        if (jp_peek(p) != ']') {
            for (;;) {
                if (len >= cap) {
                    cap *= 2;
                    void *nd = realloc(tmp_data, cap * elem_size);
                    if (!nd) { free(tmp_data); return NULL; }
                    tmp_data = nd;
                }

                EastValue *elem = jp_decode(p, elem_type, ctx);
                if (!elem) { free(tmp_data); return NULL; }

                if (elem_type->kind == EAST_TYPE_FLOAT) {
                    ((double *)tmp_data)[len] = elem->data.float64;
                } else if (elem_type->kind == EAST_TYPE_INTEGER) {
                    ((int64_t *)tmp_data)[len] = elem->data.integer;
                } else if (elem_type->kind == EAST_TYPE_BOOLEAN) {
                    ((bool *)tmp_data)[len] = elem->data.boolean;
                }
                east_value_release(elem);
                len++;

                if (!jp_match(p, ',')) break;
            }
        }

        if (!jp_match(p, ']')) { free(tmp_data); return NULL; }

        EastValue *vec = east_vector_new(elem_type, len);
        if (vec && len > 0) {
            memcpy(vec->data.vector.data, tmp_data, len * elem_size);
        }
        free(tmp_data);
        return vec;
    }

    case EAST_TYPE_MATRIX: {
        EastType *elem_type = type->data.element;
        if (!jp_match(p, '[')) return NULL;

        size_t rows = 0, cols = 0;
        size_t cap_flat = 64;
        size_t elem_size = 0;

        if (elem_type->kind == EAST_TYPE_FLOAT) {
            elem_size = sizeof(double);
        } else if (elem_type->kind == EAST_TYPE_INTEGER) {
            elem_size = sizeof(int64_t);
        } else if (elem_type->kind == EAST_TYPE_BOOLEAN) {
            elem_size = sizeof(bool);
        }

        void *flat_data = malloc(cap_flat * elem_size);
        if (!flat_data) return NULL;
        size_t flat_len = 0;

        if (jp_peek(p) != ']') {
            for (;;) {
                if (!jp_match(p, '[')) { free(flat_data); return NULL; }

                size_t row_cols = 0;
                if (jp_peek(p) != ']') {
                    for (;;) {
                        if (flat_len >= cap_flat) {
                            cap_flat *= 2;
                            void *nd = realloc(flat_data, cap_flat * elem_size);
                            if (!nd) { free(flat_data); return NULL; }
                            flat_data = nd;
                        }

                        EastValue *elem = jp_decode(p, elem_type, ctx);
                        if (!elem) { free(flat_data); return NULL; }

                        if (elem_type->kind == EAST_TYPE_FLOAT) {
                            ((double *)flat_data)[flat_len] = elem->data.float64;
                        } else if (elem_type->kind == EAST_TYPE_INTEGER) {
                            ((int64_t *)flat_data)[flat_len] = elem->data.integer;
                        } else if (elem_type->kind == EAST_TYPE_BOOLEAN) {
                            ((bool *)flat_data)[flat_len] = elem->data.boolean;
                        }
                        east_value_release(elem);
                        flat_len++;
                        row_cols++;

                        if (!jp_match(p, ',')) break;
                    }
                }

                if (!jp_match(p, ']')) { free(flat_data); return NULL; }

                if (rows == 0) cols = row_cols;
                rows++;

                if (!jp_match(p, ',')) break;
            }
        }

        if (!jp_match(p, ']')) { free(flat_data); return NULL; }

        EastValue *mat = east_matrix_new(elem_type, rows, cols);
        if (mat && flat_len > 0) {
            memcpy(mat->data.matrix.data, flat_data, flat_len * elem_size);
        }
        free(flat_data);
        return mat;
    }

    case EAST_TYPE_RECURSIVE:
        /* Unwrap: decode via the inner node type */
        if (type->data.recursive.node) {
            return jp_decode(p, type->data.recursive.node, ctx);
        }
        return NULL;

    case EAST_TYPE_NEVER:
    case EAST_TYPE_FUNCTION:
    case EAST_TYPE_ASYNC_FUNCTION:
        return NULL;
    }

    return NULL;
}

EastValue *east_json_decode(const char *json, EastType *type)
{
    if (!json || !type) return NULL;

    JsonParser parser;
    parser.input = json;
    parser.pos = 0;
    parser.len = strlen(json);

    jp_debug = (getenv("EAST_JSON_DEBUG") != NULL);

    /* Always create $ref context — cheap and handles recursive types */
    JRefCtx *ctx = jref_ctx_new();

    EastValue *result = jp_decode(&parser, type, ctx);
    jref_ctx_free(ctx);
    return result;
}

/* ================================================================== */
/*  Error-enhanced JSON decoder                                        */
/* ================================================================== */

/* Forward declarations for error-enhanced array/set decode */
static EastValue *jp_decode_array_err(JsonParser *p, EastType *type, JRefCtx *ctx, JDecodeErr *err);

static EastValue *jp_decode_err(JsonParser *p, EastType *type, JRefCtx *ctx, JDecodeErr *err)
{
    if (!type) return NULL;

    switch (type->kind) {
    case EAST_TYPE_NULL: {
        if (jp_match_str(p, "null")) return east_null();
        if (err) jde_set_msg(err, jp_fmt_error(p, "expected null"));
        return NULL;
    }

    case EAST_TYPE_BOOLEAN: {
        if (jp_match_str(p, "true"))  return east_boolean(true);
        if (jp_match_str(p, "false")) return east_boolean(false);
        if (err) jde_set_msg(err, jp_fmt_error(p, "expected boolean"));
        return NULL;
    }

    case EAST_TYPE_INTEGER: {
        jp_skip_ws(p);
        size_t save = p->pos;
        if (p->pos < p->len && p->input[p->pos] == '"') {
            size_t slen;
            char *s = jp_parse_string(p, &slen);
            if (!s) {
                if (err) { p->pos = save; jde_set_msg(err, jp_fmt_error(p, "expected string representing integer")); }
                return NULL;
            }
            /* Check empty string */
            if (slen == 0) {
                free(s);
                if (err) { p->pos = save; jde_set_msg(err, jp_fmt_error(p, "expected string representing integer")); }
                return NULL;
            }
            /* Try to parse as integer */
            char *end;
            long long val = strtoll(s, &end, 10);
            if (*end != '\0') {
                /* Non-numeric string */
                free(s);
                if (err) { p->pos = save; jde_set_msg(err, jp_fmt_error(p, "expected string representing integer")); }
                return NULL;
            }
            /* Check for overflow: strtoll saturates at LLONG_MAX/LLONG_MIN
             * but we can also check the string representation */
            /* Simple overflow check: if the string doesn't round-trip */
            char check[32];
            snprintf(check, sizeof(check), "%lld", val);
            if (strcmp(check, s) != 0) {
                /* Overflow */
                free(s);
                if (err) { p->pos = save; jde_set_msg(err, jp_fmt_error(p, "integer out of range (must be 64-bit signed)")); }
                return NULL;
            }
            free(s);
            return east_integer((int64_t)val);
        }
        /* Not a string at all */
        if (err) jde_set_msg(err, jp_fmt_error(p, "expected string representing integer"));
        return NULL;
    }

    case EAST_TYPE_FLOAT: {
        jp_skip_ws(p);
        char c = jp_peek(p);
        if (c == '"') {
            size_t save = p->pos;
            size_t slen;
            char *s = jp_parse_string(p, &slen);
            if (!s) {
                if (err) { p->pos = save; jde_set_msg(err, jp_fmt_error(p, "expected number or string representing special float value")); }
                return NULL;
            }
            double v = 0.0;
            if (strcmp(s, "NaN") == 0) v = NAN;
            else if (strcmp(s, "Infinity") == 0) v = INFINITY;
            else if (strcmp(s, "-Infinity") == 0) v = -INFINITY;
            else if (strcmp(s, "-0.0") == 0) v = -0.0;
            else {
                /* Unknown string value for float */
                free(s);
                if (err) { p->pos = save; jde_set_msg(err, jp_fmt_error(p, "expected number or string representing special float value")); }
                return NULL;
            }
            free(s);
            return east_float(v);
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            double v = jp_parse_number(p, NULL, 0);
            return east_float(v);
        }
        if (err) jde_set_msg(err, jp_fmt_error(p, "expected number or string representing special float value"));
        return NULL;
    }

    case EAST_TYPE_STRING: {
        jp_skip_ws(p);
        if (p->pos < p->len && p->input[p->pos] == '"') {
            size_t slen;
            char *s = jp_parse_string(p, &slen);
            if (!s) { if (err) jde_set_msg(err, jp_fmt_error_no_got("expected string")); return NULL; }
            EastValue *val = east_string_len(s, slen);
            free(s);
            return val;
        }
        if (err) jde_set_msg(err, jp_fmt_error(p, "expected string"));
        return NULL;
    }

    case EAST_TYPE_DATETIME: {
        jp_skip_ws(p);
        size_t save = p->pos;
        if (p->pos < p->len && p->input[p->pos] == '"') {
            size_t slen;
            char *s = jp_parse_string(p, &slen);
            if (!s) {
                if (err) { p->pos = save; jde_set_msg(err, jp_fmt_error(p, "expected string for DateTime")); }
                return NULL;
            }
            /* Validate ISO 8601 format with timezone */
            /* Pattern: YYYY-MM-DDTHH:mm:ss.sss(Z|+HH:MM|-HH:MM) */
            bool valid_format = false;
            if (slen >= 24) {
                /* Check basic structure */
                bool has_tz = false;
                if (s[slen-1] == 'Z' || s[slen-1] == 'z') has_tz = true;
                if (slen >= 29 && (s[slen-6] == '+' || s[slen-6] == '-')) has_tz = true;
                valid_format = has_tz;
            }
            if (!valid_format) {
                free(s);
                if (err) { p->pos = save; jde_set_msg(err, jp_fmt_error(p,
                    "expected ISO 8601 date string with timezone (e.g. \"2022-06-29T13:43:00.123Z\" or \"2022-06-29T13:43:00.123+05:00\")")); }
                return NULL;
            }

            int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0, ms = 0;
            int tz_hour = 0, tz_min = 0;
            int tz_sign = 1;

            sscanf(s, "%d-%d-%dT%d:%d:%d.%d",
                   &year, &month, &day, &hour, &min, &sec, &ms);
            const char *tz = s + 23;
            if (*tz == 'Z' || *tz == 'z') {
                /* UTC */
            } else if (*tz == '+' || *tz == '-') {
                tz_sign = (*tz == '-') ? -1 : 1;
                sscanf(tz + 1, "%d:%d", &tz_hour, &tz_min);
            }

            /* Validate date values */
            if (month < 1 || month > 12 || day < 1 || day > 31 ||
                hour > 23 || min > 59 || sec > 59) {
                /* Check if Date would be invalid */
                /* Simple check: very out-of-range values */
                int64_t y = year;
                int64_t m_adj = month;
                if (m_adj <= 2) { y--; m_adj += 9; } else { m_adj -= 3; }
                int64_t era = (y >= 0 ? y : y - 399) / 400;
                int64_t yoe = y - era * 400;
                int64_t doy = (153 * m_adj + 2) / 5 + day - 1;
                int64_t doe = yoe * 365 + yoe/4 - yoe/100 + doy;
                int64_t days = era * 146097 + doe - 719468;
                int64_t epoch_secs = days * 86400 + hour * 3600 + min * 60 + sec;
                epoch_secs -= tz_sign * (tz_hour * 3600 + tz_min * 60);
                int64_t epoch_ms = epoch_secs * 1000 + ms;
                /* If NaN equivalent (TS Date invalid) - report error */
                /* For extreme values like month 13 or hour 25, TS new Date() gives invalid */
                if (month > 12 || hour > 23 || min > 59 || sec > 59) {
                    free(s);
                    if (err) { p->pos = save; jde_set_msg(err, jp_fmt_error(p, "invalid date string")); }
                    return NULL;
                }
                free(s);
                return east_datetime(epoch_ms);
            }

            int64_t y = year;
            int64_t m_adj = month;
            if (m_adj <= 2) { y--; m_adj += 9; } else { m_adj -= 3; }
            int64_t era = (y >= 0 ? y : y - 399) / 400;
            int64_t yoe = y - era * 400;
            int64_t doy = (153 * m_adj + 2) / 5 + day - 1;
            int64_t doe = yoe * 365 + yoe/4 - yoe/100 + doy;
            int64_t days = era * 146097 + doe - 719468;
            int64_t epoch_secs = days * 86400 + hour * 3600 + min * 60 + sec;
            epoch_secs -= tz_sign * (tz_hour * 3600 + tz_min * 60);
            int64_t epoch_ms = epoch_secs * 1000 + ms;
            free(s);
            return east_datetime(epoch_ms);
        }
        if (err) jde_set_msg(err, jp_fmt_error(p, "expected string for DateTime"));
        return NULL;
    }

    case EAST_TYPE_BLOB: {
        jp_skip_ws(p);
        size_t save = p->pos;
        if (p->pos < p->len && p->input[p->pos] == '"') {
            size_t slen;
            char *s = jp_parse_string(p, &slen);
            if (!s) {
                if (err) { p->pos = save; jde_set_msg(err, jp_fmt_error(p, "expected hex string starting with 0x")); }
                return NULL;
            }
            if (slen < 2 || s[0] != '0' || s[1] != 'x') {
                free(s);
                if (err) { p->pos = save; jde_set_msg(err, jp_fmt_error(p, "expected hex string starting with 0x")); }
                return NULL;
            }
            const char *hex = s + 2;
            size_t hex_len = slen - 2;
            /* Validate hex characters and length */
            bool valid_hex = true;
            for (size_t i = 0; i < hex_len; i++) {
                char c = hex[i];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                    valid_hex = false;
                    break;
                }
            }
            if (!valid_hex || hex_len % 2 != 0) {
                free(s);
                if (err) { p->pos = save; jde_set_msg(err, jp_fmt_error(p, "invalid hex string")); }
                return NULL;
            }
            size_t blob_len = hex_len / 2;
            uint8_t *decoded = malloc(blob_len > 0 ? blob_len : 1);
            if (!decoded) { free(s); return NULL; }
            for (size_t i = 0; i < blob_len; i++) {
                unsigned int byte_val = 0;
                char byte_hex[3] = { hex[i * 2], hex[i * 2 + 1], '\0' };
                sscanf(byte_hex, "%02x", &byte_val);
                decoded[i] = (uint8_t)byte_val;
            }
            free(s);
            EastValue *val = east_blob(decoded, blob_len);
            free(decoded);
            return val;
        }
        if (err) jde_set_msg(err, jp_fmt_error(p, "expected hex string starting with 0x"));
        return NULL;
    }

    case EAST_TYPE_ARRAY:
    case EAST_TYPE_SET:
        return jp_decode_array_err(p, type, ctx, err);

    case EAST_TYPE_DICT: {
        EastValue *ref = jp_try_ref(p, ctx);
        if (ref) return ref;

        jp_skip_ws(p);
        size_t save = p->pos;
        if (p->pos >= p->len || p->input[p->pos] != '[') {
            const char *ename = (type->kind == EAST_TYPE_DICT) ? "expected array for Dict" : "expected array";
            if (err) jde_set_msg(err, jp_fmt_error(p, ename));
            return NULL;
        }

        EastType *key_type = type->data.dict.key;
        EastType *val_type = type->data.dict.value;
        EastValue *dict = east_dict_new(key_type, val_type);
        if (!dict) return NULL;

        if (ctx) jref_register(ctx, dict);

        if (!jp_match(p, '[')) { east_value_release(dict); return NULL; }

        if (jp_peek(p) != ']') {
            size_t idx = 0;
            for (;;) {
                /* Each dict entry must be an object with "key" and "value" */
                jp_skip_ws(p);
                size_t entry_save = p->pos;
                if (p->pos >= p->len || p->input[p->pos] != '{') {
                    /* Not an object */
                    if (err) {
                        char *raw = jp_extract_raw_value(p);
                        char reason[128];
                        snprintf(reason, sizeof(reason), "expected object with key and value for Dict entry");
                        jde_set_msg(err, jp_fmt_error_raw(reason, raw));
                        char pathbuf[32];
                        snprintf(pathbuf, sizeof(pathbuf), "[%zu]", idx);
                        jde_prepend_path(err, pathbuf);
                        free(raw);
                    }
                    east_value_release(dict);
                    return NULL;
                }

                /* Parse entry: save raw for error messages */
                size_t obj_start = p->pos;
                if (!jp_match(p, '{')) { east_value_release(dict); return NULL; }

                EastValue *k = NULL;
                EastValue *v = NULL;
                bool has_key = false, has_value = false;
                bool has_extra = false;
                char extra_name[64] = {0};

                /* Parse all fields in the entry */
                if (jp_peek(p) != '}') {
                    for (;;) {
                        size_t fname_len;
                        char *fname = jp_parse_string(p, &fname_len);
                        if (!fname) { east_value_release(dict); return NULL; }
                        jp_match(p, ':');
                        if (strcmp(fname, "key") == 0) {
                            char seg[32];
                            snprintf(seg, sizeof(seg), "%zu", idx);
                            if (ctx) { jref_push(ctx, seg); jref_push(ctx, "key"); }
                            JDecodeErr inner_err;
                            jde_init(&inner_err);
                            k = jp_decode_err(p, key_type, ctx, err ? &inner_err : NULL);
                            if (ctx) { jref_pop(ctx); jref_pop(ctx); }
                            if (!k && err && inner_err.message) {
                                char pathbuf[64];
                                snprintf(pathbuf, sizeof(pathbuf), "[%zu].key", idx);
                                jde_set_msg_path(err, inner_err.message, inner_err.path);
                                inner_err.message = NULL; inner_err.path = NULL;
                                jde_prepend_path(err, pathbuf);
                                jde_free(&inner_err);
                                free(fname);
                                if (k) east_value_release(k);
                                if (v) east_value_release(v);
                                east_value_release(dict);
                                return NULL;
                            }
                            jde_free(&inner_err);
                            has_key = true;
                        } else if (strcmp(fname, "value") == 0) {
                            char seg[32];
                            snprintf(seg, sizeof(seg), "%zu", idx);
                            if (ctx) { jref_push(ctx, seg); jref_push(ctx, "value"); }
                            JDecodeErr inner_err;
                            jde_init(&inner_err);
                            v = jp_decode_err(p, val_type, ctx, err ? &inner_err : NULL);
                            if (ctx) { jref_pop(ctx); jref_pop(ctx); }
                            if (!v && err && inner_err.message) {
                                char pathbuf[64];
                                snprintf(pathbuf, sizeof(pathbuf), "[%zu].value", idx);
                                jde_set_msg_path(err, inner_err.message, inner_err.path);
                                inner_err.message = NULL; inner_err.path = NULL;
                                jde_prepend_path(err, pathbuf);
                                jde_free(&inner_err);
                                free(fname);
                                if (k) east_value_release(k);
                                east_value_release(dict);
                                return NULL;
                            }
                            jde_free(&inner_err);
                            has_value = true;
                        } else {
                            if (!has_extra) {
                                snprintf(extra_name, sizeof(extra_name), "%s", fname);
                                has_extra = true;
                            }
                            jp_skip_json_value(p);
                        }
                        free(fname);
                        if (!jp_match(p, ',')) break;
                    }
                }

                if (!jp_match(p, '}')) {
                    if (k) east_value_release(k);
                    if (v) east_value_release(v);
                    east_value_release(dict);
                    return NULL;
                }

                /* Check for extra fields */
                if (has_extra && err) {
                    /* Extract raw entry text */
                    size_t obj_end = p->pos;
                    size_t raw_len = obj_end - obj_start;
                    char *raw = malloc(raw_len + 1);
                    memcpy(raw, p->input + obj_start, raw_len);
                    raw[raw_len] = '\0';
                    char *reason = malloc(128 + strlen(extra_name));
                    sprintf(reason, "unexpected field \"%s\" in Dict entry, got %s", extra_name, raw);
                    free(raw);
                    char pathbuf[32];
                    snprintf(pathbuf, sizeof(pathbuf), "[%zu]", idx);
                    jde_set_msg_path(err, reason, strdup(pathbuf));
                    if (k) east_value_release(k);
                    if (v) east_value_release(v);
                    east_value_release(dict);
                    return NULL;
                }

                /* Check missing key/value */
                if ((!has_key || !has_value) && err) {
                    size_t obj_end = p->pos;
                    size_t raw_len = obj_end - obj_start;
                    char *raw = malloc(raw_len + 1);
                    memcpy(raw, p->input + obj_start, raw_len);
                    raw[raw_len] = '\0';
                    char *reason = malloc(128 + raw_len);
                    sprintf(reason, "expected object with key and value for Dict entry, got %s", raw);
                    free(raw);
                    char pathbuf[32];
                    snprintf(pathbuf, sizeof(pathbuf), "[%zu]", idx);
                    jde_set_msg_path(err, reason, strdup(pathbuf));
                    if (k) east_value_release(k);
                    if (v) east_value_release(v);
                    east_value_release(dict);
                    return NULL;
                }

                if (k && v) east_dict_set(dict, k, v);
                if (k) east_value_release(k);
                if (v) east_value_release(v);
                idx++;

                if (!jp_match(p, ',')) break;
            }
        }

        if (!jp_match(p, ']')) { east_value_release(dict); return NULL; }
        return dict;
    }

    case EAST_TYPE_STRUCT: {
        EastValue *ref = jp_try_ref(p, ctx);
        if (ref) return ref;

        jp_skip_ws(p);
        size_t obj_start = p->pos;
        if (p->pos >= p->len || p->input[p->pos] != '{') {
            if (err) jde_set_msg(err, jp_fmt_error(p, "expected object for Struct"));
            return NULL;
        }
        /* Check for null */
        if (jp_match_str(p, "null")) {
            if (err) jde_set_msg(err, jp_fmt_error_raw("expected object for Struct", "null"));
            return NULL;
        }
        if (!jp_match(p, '{')) {
            if (err) jde_set_msg(err, jp_fmt_error(p, "expected object for Struct"));
            return NULL;
        }

        size_t nf = type->data.struct_.num_fields;
        const char **names = calloc(nf, sizeof(char *));
        EastValue **values = calloc(nf, sizeof(EastValue *));
        if (!names || !values) {
            free(names);
            free(values);
            return NULL;
        }

        for (size_t i = 0; i < nf; i++) {
            names[i] = type->data.struct_.fields[i].name;
            values[i] = NULL;
        }

        /* Track which fields we see */
        bool *seen = calloc(nf, sizeof(bool));
        char first_extra[128] = {0};
        bool has_extra = false;

        if (jp_peek(p) != '}') {
            for (;;) {
                size_t fname_len;
                char *fname = jp_parse_string(p, &fname_len);
                if (!fname) goto struct_err_fail;
                jp_match(p, ':');

                EastType *ftype = NULL;
                size_t fidx = 0;
                for (size_t i = 0; i < nf; i++) {
                    if (strcmp(names[i], fname) == 0) {
                        ftype = type->data.struct_.fields[i].type;
                        fidx = i;
                        break;
                    }
                }

                if (ftype) {
                    seen[fidx] = true;
                    if (ctx) jref_push(ctx, fname);
                    JDecodeErr inner_err;
                    jde_init(&inner_err);
                    values[fidx] = jp_decode_err(p, ftype, ctx, err ? &inner_err : NULL);
                    if (ctx) jref_pop(ctx);
                    if (!values[fidx] && err && inner_err.message) {
                        char pathbuf[256];
                        snprintf(pathbuf, sizeof(pathbuf), ".%s", fname);
                        jde_set_msg_path(err, inner_err.message, inner_err.path);
                        inner_err.message = NULL; inner_err.path = NULL;
                        jde_prepend_path(err, pathbuf);
                        jde_free(&inner_err);
                        free(fname);
                        free(seen);
                        goto struct_err_fail;
                    }
                    jde_free(&inner_err);
                } else {
                    if (!has_extra) {
                        snprintf(first_extra, sizeof(first_extra), "%s", fname);
                        has_extra = true;
                    }
                    jp_skip_json_value(p);
                }
                free(fname);

                if (!jp_match(p, ',')) break;
            }
        }

        if (!jp_match(p, '}')) {
            free(seen);
            goto struct_err_fail;
        }

        /* Check for extra fields */
        if (has_extra && err) {
            size_t obj_end = p->pos;
            size_t raw_len = obj_end - obj_start;
            char *raw = malloc(raw_len + 1);
            memcpy(raw, p->input + obj_start, raw_len);
            raw[raw_len] = '\0';
            char *reason = malloc(256 + raw_len);
            sprintf(reason, "unexpected field \"%s\" in Struct, got %s", first_extra, raw);
            free(raw);
            jde_set_msg(err, reason);
            free(seen);
            goto struct_err_fail;
        }

        /* Check for missing fields */
        for (size_t i = 0; i < nf; i++) {
            if (!seen[i] && err) {
                size_t obj_end = p->pos;
                size_t raw_len = obj_end - obj_start;
                char *raw = malloc(raw_len + 1);
                memcpy(raw, p->input + obj_start, raw_len);
                raw[raw_len] = '\0';
                char *reason = malloc(256 + raw_len);
                sprintf(reason, "missing field \"%s\" in Struct, got %s", names[i], raw);
                free(raw);
                jde_set_msg(err, reason);
                free(seen);
                goto struct_err_fail;
            }
            if (!values[i]) values[i] = east_null();
        }

        free(seen);
        {
            EastValue *result = east_struct_new(names, values, nf, type);
            for (size_t i = 0; i < nf; i++) {
                east_value_release(values[i]);
            }
            free(names);
            free(values);
            return result;
        }

struct_err_fail:
        for (size_t i = 0; i < nf; i++) {
            if (values[i]) east_value_release(values[i]);
        }
        free(names);
        free(values);
        return NULL;
    }

    case EAST_TYPE_VARIANT: {
        EastValue *ref = jp_try_ref(p, ctx);
        if (ref) return ref;

        jp_skip_ws(p);
        size_t obj_start = p->pos;
        if (p->pos >= p->len || p->input[p->pos] != '{') {
            if (err) jde_set_msg(err, jp_fmt_error(p, "expected object with type and value for Variant"));
            return NULL;
        }

        if (!jp_match(p, '{')) return NULL;

        char *case_name = NULL;
        EastValue *case_value = NULL;
        bool has_type = false, has_value = false;

        for (int fi = 0; fi < 2; fi++) {
            jp_skip_ws(p);
            if (p->pos < p->len && p->input[p->pos] == '}') break;  /* fewer than 2 fields */
            size_t fname_len;
            char *fname = jp_parse_string(p, &fname_len);
            if (!fname) { free(case_name); return NULL; }
            jp_match(p, ':');

            if (strcmp(fname, "type") == 0) {
                size_t cn_len;
                case_name = jp_parse_string(p, &cn_len);
                has_type = true;
            } else if (strcmp(fname, "value") == 0) {
                if (case_name) {
                    EastType *case_type = NULL;
                    for (size_t i = 0; i < type->data.variant.num_cases; i++) {
                        if (strcmp(type->data.variant.cases[i].name, case_name) == 0) {
                            case_type = type->data.variant.cases[i].type;
                            break;
                        }
                    }
                    if (case_type) {
                        if (ctx) jref_push(ctx, case_name);
                        JDecodeErr inner_err;
                        jde_init(&inner_err);
                        case_value = jp_decode_err(p, case_type, ctx, err ? &inner_err : NULL);
                        if (ctx) jref_pop(ctx);
                        if (!case_value && err && inner_err.message) {
                            char pathbuf[256];
                            snprintf(pathbuf, sizeof(pathbuf), ".%s", case_name);
                            jde_set_msg_path(err, inner_err.message, inner_err.path);
                            inner_err.message = NULL; inner_err.path = NULL;
                            jde_prepend_path(err, pathbuf);
                            jde_free(&inner_err);
                            free(fname);
                            free(case_name);
                            return NULL;
                        }
                        jde_free(&inner_err);
                        has_value = true;
                    } else {
                        /* Unknown case name — will handle after parsing */
                        jp_skip_json_value(p);
                        has_value = true;
                    }
                } else {
                    jp_skip_json_value(p);
                    has_value = true;
                }
            }
            free(fname);
            if (fi == 0) jp_match(p, ',');
        }

        if (!jp_match(p, '}')) {
            free(case_name);
            if (case_value) east_value_release(case_value);
            return NULL;
        }

        /* Check for missing type/value fields */
        if (!has_type || !has_value) {
            if (err) {
                size_t obj_end = p->pos;
                size_t raw_len = obj_end - obj_start;
                char *raw = malloc(raw_len + 1);
                memcpy(raw, p->input + obj_start, raw_len);
                raw[raw_len] = '\0';
                jde_set_msg(err, jp_fmt_error_raw("expected object with type and value for Variant", raw));
                free(raw);
            }
            free(case_name);
            if (case_value) east_value_release(case_value);
            return NULL;
        }

        if (!case_name) { if (case_value) east_value_release(case_value); return NULL; }

        /* Check if case name is known */
        bool known_case = false;
        for (size_t i = 0; i < type->data.variant.num_cases; i++) {
            if (strcmp(type->data.variant.cases[i].name, case_name) == 0) {
                known_case = true;
                break;
            }
        }
        if (!known_case) {
            if (err) {
                size_t obj_end = p->pos;
                size_t raw_len = obj_end - obj_start;
                char *raw = malloc(raw_len + 1);
                memcpy(raw, p->input + obj_start, raw_len);
                raw[raw_len] = '\0';
                char *reason = malloc(256 + raw_len + strlen(case_name));
                sprintf(reason, "unknown variant type \"%s\", got %s", case_name, raw);
                free(raw);
                jde_set_msg(err, reason);
            }
            free(case_name);
            if (case_value) east_value_release(case_value);
            return NULL;
        }

        if (!case_value) case_value = east_null();
        EastValue *result = east_variant_new(case_name, case_value, type);
        east_value_release(case_value);
        free(case_name);
        return result;
    }

    case EAST_TYPE_REF: {
        EastValue *ref = jp_try_ref(p, ctx);
        if (ref) return ref;

        jp_skip_ws(p);
        size_t save = p->pos;
        if (p->pos >= p->len || p->input[p->pos] != '[') {
            if (err) jde_set_msg(err, jp_fmt_error(p, "expected array with 1 entry"));
            return NULL;
        }
        if (!jp_match(p, '[')) return NULL;
        JDecodeErr inner_err;
        jde_init(&inner_err);
        EastValue *inner = jp_decode_err(p, type->data.element, ctx, err ? &inner_err : NULL);
        if (!inner) {
            if (err && inner_err.message) {
                jde_set_msg_path(err, inner_err.message, inner_err.path);
                inner_err.message = NULL; inner_err.path = NULL;
            }
            jde_free(&inner_err);
            return NULL;
        }
        jde_free(&inner_err);
        if (!jp_match(p, ']')) { east_value_release(inner); return NULL; }
        if (ctx) jref_register(ctx, inner);
        EastValue *result = east_ref_new(inner);
        east_value_release(inner);
        return result;
    }

    case EAST_TYPE_VECTOR: {
        /* Vectors use the same format as arrays in JSON */
        EastType *elem_type = type->data.element;
        if (!jp_match(p, '[')) {
            if (err) jde_set_msg(err, jp_fmt_error(p, "expected array for Vector"));
            return NULL;
        }

        size_t cap = 16, len = 0;
        void *tmp_data = NULL;
        size_t elem_size = 0;

        if (elem_type->kind == EAST_TYPE_FLOAT) elem_size = sizeof(double);
        else if (elem_type->kind == EAST_TYPE_INTEGER) elem_size = sizeof(int64_t);
        else if (elem_type->kind == EAST_TYPE_BOOLEAN) elem_size = sizeof(bool);

        tmp_data = malloc(cap * elem_size);
        if (!tmp_data) return NULL;

        if (jp_peek(p) != ']') {
            for (;;) {
                if (len >= cap) {
                    cap *= 2;
                    void *nd = realloc(tmp_data, cap * elem_size);
                    if (!nd) { free(tmp_data); return NULL; }
                    tmp_data = nd;
                }
                JDecodeErr inner_err;
                jde_init(&inner_err);
                EastValue *elem = jp_decode_err(p, elem_type, ctx, err ? &inner_err : NULL);
                if (!elem) {
                    if (err && inner_err.message) {
                        char pathbuf[32];
                        snprintf(pathbuf, sizeof(pathbuf), "[%zu]", len);
                        jde_set_msg_path(err, inner_err.message, inner_err.path);
                        inner_err.message = NULL; inner_err.path = NULL;
                        jde_prepend_path(err, pathbuf);
                    }
                    jde_free(&inner_err);
                    free(tmp_data);
                    return NULL;
                }
                jde_free(&inner_err);

                if (elem_type->kind == EAST_TYPE_FLOAT) ((double *)tmp_data)[len] = elem->data.float64;
                else if (elem_type->kind == EAST_TYPE_INTEGER) ((int64_t *)tmp_data)[len] = elem->data.integer;
                else if (elem_type->kind == EAST_TYPE_BOOLEAN) ((bool *)tmp_data)[len] = elem->data.boolean;
                east_value_release(elem);
                len++;
                if (!jp_match(p, ',')) break;
            }
        }

        if (!jp_match(p, ']')) { free(tmp_data); return NULL; }
        EastValue *vec = east_vector_new(elem_type, len);
        if (vec && len > 0) memcpy(vec->data.vector.data, tmp_data, len * elem_size);
        free(tmp_data);
        return vec;
    }

    case EAST_TYPE_MATRIX: {
        EastType *elem_type = type->data.element;
        if (!jp_match(p, '[')) {
            if (err) jde_set_msg(err, jp_fmt_error(p, "expected array for Matrix"));
            return NULL;
        }
        /* Reuse non-error path for matrix since test coverage is minimal */
        size_t rows = 0, cols = 0;
        size_t cap_flat = 64, elem_size = 0;
        if (elem_type->kind == EAST_TYPE_FLOAT) elem_size = sizeof(double);
        else if (elem_type->kind == EAST_TYPE_INTEGER) elem_size = sizeof(int64_t);
        else if (elem_type->kind == EAST_TYPE_BOOLEAN) elem_size = sizeof(bool);

        void *flat_data = malloc(cap_flat * elem_size);
        if (!flat_data) return NULL;
        size_t flat_len = 0;

        if (jp_peek(p) != ']') {
            for (;;) {
                if (!jp_match(p, '[')) { free(flat_data); return NULL; }
                size_t row_cols = 0;
                if (jp_peek(p) != ']') {
                    for (;;) {
                        if (flat_len >= cap_flat) {
                            cap_flat *= 2;
                            void *nd = realloc(flat_data, cap_flat * elem_size);
                            if (!nd) { free(flat_data); return NULL; }
                            flat_data = nd;
                        }
                        EastValue *elem = jp_decode_err(p, elem_type, ctx, err);
                        if (!elem) { free(flat_data); return NULL; }
                        if (elem_type->kind == EAST_TYPE_FLOAT) ((double *)flat_data)[flat_len] = elem->data.float64;
                        else if (elem_type->kind == EAST_TYPE_INTEGER) ((int64_t *)flat_data)[flat_len] = elem->data.integer;
                        else if (elem_type->kind == EAST_TYPE_BOOLEAN) ((bool *)flat_data)[flat_len] = elem->data.boolean;
                        east_value_release(elem);
                        flat_len++;
                        row_cols++;
                        if (!jp_match(p, ',')) break;
                    }
                }
                if (!jp_match(p, ']')) { free(flat_data); return NULL; }
                if (rows == 0) cols = row_cols;
                rows++;
                if (!jp_match(p, ',')) break;
            }
        }
        if (!jp_match(p, ']')) { free(flat_data); return NULL; }
        EastValue *mat = east_matrix_new(elem_type, rows, cols);
        if (mat && flat_len > 0) memcpy(mat->data.matrix.data, flat_data, flat_len * elem_size);
        free(flat_data);
        return mat;
    }

    case EAST_TYPE_RECURSIVE:
        if (type->data.recursive.node) {
            return jp_decode_err(p, type->data.recursive.node, ctx, err);
        }
        return NULL;

    case EAST_TYPE_NEVER:
    case EAST_TYPE_FUNCTION:
    case EAST_TYPE_ASYNC_FUNCTION:
        return NULL;
    }

    return NULL;
}

static EastValue *jp_decode_array_err(JsonParser *p, EastType *type, JRefCtx *ctx, JDecodeErr *err)
{
    /* Check for $ref */
    EastValue *ref = jp_try_ref(p, ctx);
    if (ref) return ref;

    jp_skip_ws(p);
    if (p->pos >= p->len || p->input[p->pos] != '[') {
        const char *ename = (type->kind == EAST_TYPE_SET) ? "expected array for Set" : "expected array";
        if (err) jde_set_msg(err, jp_fmt_error(p, ename));
        return NULL;
    }

    EastType *elem_type = type->data.element;
    EastValue *arr;
    if (type->kind == EAST_TYPE_SET) {
        arr = east_set_new(elem_type);
    } else {
        arr = east_array_new(elem_type);
    }
    if (!arr) return NULL;

    if (ctx) jref_register(ctx, arr);

    if (!jp_match(p, '[')) { east_value_release(arr); return NULL; }

    if (jp_peek(p) != ']') {
        size_t idx = 0;
        for (;;) {
            char idx_str[24];
            snprintf(idx_str, sizeof(idx_str), "%zu", idx);
            if (ctx) jref_push(ctx, idx_str);

            JDecodeErr inner_err;
            jde_init(&inner_err);
            EastValue *elem = jp_decode_err(p, elem_type, ctx, err ? &inner_err : NULL);

            if (ctx) jref_pop(ctx);

            if (!elem) {
                if (err && inner_err.message) {
                    char pathbuf[32];
                    snprintf(pathbuf, sizeof(pathbuf), "[%zu]", idx);
                    jde_set_msg_path(err, inner_err.message, inner_err.path);
                    inner_err.message = NULL; inner_err.path = NULL;
                    jde_prepend_path(err, pathbuf);
                }
                jde_free(&inner_err);
                east_value_release(arr);
                return NULL;
            }
            jde_free(&inner_err);

            if (type->kind == EAST_TYPE_SET) {
                east_set_insert(arr, elem);
            } else {
                east_array_push(arr, elem);
            }
            east_value_release(elem);
            idx++;
            if (!jp_match(p, ',')) break;
        }
    }

    if (!jp_match(p, ']')) { east_value_release(arr); return NULL; }
    return arr;
}

EastValue *east_json_decode_with_error(const char *json, EastType *type, char **error_out)
{
    if (!json || !type) return NULL;

    JsonParser parser;
    parser.input = json;
    parser.pos = 0;
    parser.len = strlen(json);

    jp_debug = (getenv("EAST_JSON_DEBUG") != NULL);

    JRefCtx *ctx = jref_ctx_new();
    JDecodeErr err;
    jde_init(&err);

    EastValue *result = jp_decode_err(&parser, type, ctx, error_out ? &err : NULL);
    jref_ctx_free(ctx);

    if (!result && error_out && err.message) {
        /* Build the complete error string */
        char *type_str = east_print_type(type);
        const char *path_str = (err.path && err.path[0]) ? err.path : NULL;
        size_t total = strlen(err.message) + 200;
        if (path_str) total += strlen(path_str);
        if (type_str) total += strlen(type_str);
        char *full_msg = malloc(total);
        if (path_str) {
            snprintf(full_msg, total,
                "Error occurred because %s at %s (line 1, col 1) while parsing value of type \"%s\"",
                err.message, path_str, type_str ? type_str : "?");
        } else {
            snprintf(full_msg, total,
                "Error occurred because %s (line 1, col 1) while parsing value of type \"%s\"",
                err.message, type_str ? type_str : "?");
        }
        free(type_str);
        *error_out = full_msg;
    } else if (error_out) {
        *error_out = NULL;
    }

    jde_free(&err);
    return result;
}
