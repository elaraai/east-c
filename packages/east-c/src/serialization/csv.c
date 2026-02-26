/*
 * CSV serialization for East types (RFC 4180 compliant).
 *
 * Provides encoding/decoding for Array<Struct> to/from CSV format.
 * Type-driven: the struct type guides how field values are
 * encoded/decoded to/from CSV string cells.
 *
 * Supported field types: Null, Boolean, Integer, Float, String, DateTime, Blob.
 * Fields may also be OptionType (Variant with none/some) wrapping a supported type.
 *
 * Options are passed as EastValue structs with Option<T> fields, matching
 * the TypeScript CsvParseConfigType and CsvSerializeConfigType.
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
} CsvBuf;

static CsvBuf csvbuf_new(size_t initial_cap)
{
    CsvBuf sb;
    sb.cap = (initial_cap > 0) ? initial_cap : 256;
    sb.data = malloc(sb.cap);
    sb.len = 0;
    if (sb.data) sb.data[0] = '\0';
    return sb;
}

static void csvbuf_ensure(CsvBuf *sb, size_t needed)
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

static void csvbuf_append(CsvBuf *sb, const char *str, size_t len)
{
    if (len == 0) return;
    csvbuf_ensure(sb, len);
    memcpy(sb->data + sb->len, str, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

static void csvbuf_append_str(CsvBuf *sb, const char *str)
{
    csvbuf_append(sb, str, strlen(str));
}

static void csvbuf_append_char(CsvBuf *sb, char c)
{
    csvbuf_ensure(sb, 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

/* ================================================================== */
/*  Option type helpers                                                */
/* ================================================================== */

/* Check if a type is Option<T> = Variant { none: Null, some: T } */
static bool is_option_type(EastType *type)
{
    if (!type || type->kind != EAST_TYPE_VARIANT) return false;
    if (type->data.variant.num_cases != 2) return false;
    /* Cases are sorted alphabetically: none at 0, some at 1 */
    return strcmp(type->data.variant.cases[0].name, "none") == 0 &&
           strcmp(type->data.variant.cases[1].name, "some") == 0;
}

/* Get the inner type of Option<T> (the 'some' case type) */
static EastType *option_inner_type(EastType *type)
{
    return type->data.variant.cases[1].type;
}

/* ================================================================== */
/*  Config extraction helpers                                          */
/* ================================================================== */

/* Get an optional string field from config struct. Returns NULL if none. */
static const char *config_get_string(EastValue *config, const char *field)
{
    if (!config || config->kind != EAST_VAL_STRUCT) return NULL;
    EastValue *v = east_struct_get_field(config, field);
    if (!v || v->kind != EAST_VAL_VARIANT) return NULL;
    if (strcmp(v->data.variant.case_name, "some") != 0) return NULL;
    EastValue *inner = v->data.variant.value;
    if (!inner || inner->kind != EAST_VAL_STRING) return NULL;
    return inner->data.string.data;
}

/* Get an optional boolean field from config struct. Returns def if none. */
static bool config_get_bool(EastValue *config, const char *field, bool def)
{
    if (!config || config->kind != EAST_VAL_STRUCT) return def;
    EastValue *v = east_struct_get_field(config, field);
    if (!v || v->kind != EAST_VAL_VARIANT) return def;
    if (strcmp(v->data.variant.case_name, "some") != 0) return def;
    EastValue *inner = v->data.variant.value;
    if (!inner || inner->kind != EAST_VAL_BOOLEAN) return def;
    return inner->data.boolean;
}

/* Get an optional Dict<String,String> field from config struct.
 * Returns the dict EastValue* or NULL if none. */
static EastValue *config_get_dict(EastValue *config, const char *field)
{
    if (!config || config->kind != EAST_VAL_STRUCT) return NULL;
    EastValue *v = east_struct_get_field(config, field);
    if (!v || v->kind != EAST_VAL_VARIANT) return NULL;
    if (strcmp(v->data.variant.case_name, "some") != 0) return NULL;
    EastValue *inner = v->data.variant.value;
    if (!inner || inner->kind != EAST_VAL_DICT) return NULL;
    return inner;
}

/* Look up a string key in a Dict<String,String>. Returns the mapped value
 * string or NULL if not found. The returned pointer is borrowed. */
static const char *dict_lookup_string(EastValue *dict, const char *key)
{
    if (!dict) return NULL;
    for (size_t i = 0; i < dict->data.dict.len; i++) {
        EastValue *k = dict->data.dict.keys[i];
        if (k && k->kind == EAST_VAL_STRING &&
            strcmp(k->data.string.data, key) == 0) {
            EastValue *v = dict->data.dict.values[i];
            if (v && v->kind == EAST_VAL_STRING)
                return v->data.string.data;
            return NULL;
        }
    }
    return NULL;
}

/* Get optional nullStrings array. Returns count and sets *out.
 * Caller must not free the strings (they belong to the config value).
 * Returns -1 if not set. */
static int config_get_null_strings(EastValue *config, const char ***out)
{
    if (!config || config->kind != EAST_VAL_STRUCT) return -1;
    EastValue *v = east_struct_get_field(config, "nullStrings");
    if (!v || v->kind != EAST_VAL_VARIANT) return -1;
    if (strcmp(v->data.variant.case_name, "some") != 0) return -1;
    EastValue *arr = v->data.variant.value;
    if (!arr || arr->kind != EAST_VAL_ARRAY) return -1;
    size_t n = arr->data.array.len;
    const char **strs = malloc(n * sizeof(char *));
    if (!strs) return -1;
    for (size_t i = 0; i < n; i++) {
        EastValue *s = arr->data.array.items[i];
        strs[i] = (s && s->kind == EAST_VAL_STRING) ? s->data.string.data : "";
    }
    *out = strs;
    return (int)n;
}

/* ================================================================== */
/*  Resolved config structs                                            */
/* ================================================================== */

typedef struct {
    char delimiter;
    char quote_char;
    char escape_char;
    const char *newline;     /* "\r\n" or custom */
    bool include_header;
    const char *null_string; /* "" default */
    bool always_quote;
} CsvEncodeOpts;

typedef struct {
    char delimiter;
    char quote_char;
    char escape_char;
    bool has_header;
    const char **null_strings;  /* array of strings treated as null */
    int null_strings_count;     /* -1 = use default [""] */
    bool trim_fields;
    bool skip_empty_lines;
    bool strict;
} CsvDecodeOpts;

static CsvEncodeOpts resolve_encode_opts(EastValue *config)
{
    CsvEncodeOpts o;
    const char *s;

    s = config_get_string(config, "delimiter");
    o.delimiter = (s && s[0]) ? s[0] : ',';

    s = config_get_string(config, "quoteChar");
    o.quote_char = (s && s[0]) ? s[0] : '"';

    s = config_get_string(config, "escapeChar");
    o.escape_char = (s && s[0]) ? s[0] : '"';

    s = config_get_string(config, "newline");
    o.newline = s ? s : "\r\n";

    o.include_header = config_get_bool(config, "includeHeader", true);

    s = config_get_string(config, "nullString");
    o.null_string = s ? s : "";

    o.always_quote = config_get_bool(config, "alwaysQuote", false);

    return o;
}

static CsvDecodeOpts resolve_decode_opts(EastValue *config)
{
    CsvDecodeOpts o;
    const char *s;

    s = config_get_string(config, "delimiter");
    o.delimiter = (s && s[0]) ? s[0] : ',';

    s = config_get_string(config, "quoteChar");
    o.quote_char = (s && s[0]) ? s[0] : '"';

    s = config_get_string(config, "escapeChar");
    o.escape_char = (s && s[0]) ? s[0] : '"';

    o.has_header = config_get_bool(config, "hasHeader", true);
    o.trim_fields = config_get_bool(config, "trimFields", false);
    o.skip_empty_lines = config_get_bool(config, "skipEmptyLines", true);
    o.strict = config_get_bool(config, "strict", false);

    o.null_strings_count = config_get_null_strings(config, &o.null_strings);

    return o;
}

static void decode_opts_free(CsvDecodeOpts *o)
{
    if (o->null_strings_count >= 0) {
        free((void *)o->null_strings);
    }
}

/* Check if a string is in the null_strings list */
static bool is_null_string(const CsvDecodeOpts *o, const char *str)
{
    if (o->null_strings_count >= 0) {
        for (int i = 0; i < o->null_strings_count; i++) {
            if (strcmp(str, o->null_strings[i]) == 0) return true;
        }
        return false;
    }
    /* Default: only empty string is null */
    return str[0] == '\0';
}

/* ================================================================== */
/*  CSV quoting with configurable chars                                */
/* ================================================================== */

static bool csv_needs_quoting(const char *val, size_t len, char delim, char quote)
{
    for (size_t i = 0; i < len; i++) {
        char c = val[i];
        if (c == delim || c == quote || c == '\r' || c == '\n') return true;
    }
    return false;
}

static void csvbuf_append_quoted(CsvBuf *sb, const char *val, size_t len,
                                  char quote, char escape)
{
    csvbuf_append_char(sb, quote);
    for (size_t i = 0; i < len; i++) {
        if (val[i] == quote) {
            csvbuf_append_char(sb, escape);
            csvbuf_append_char(sb, quote);
        } else {
            csvbuf_append_char(sb, val[i]);
        }
    }
    csvbuf_append_char(sb, quote);
}

static void csvbuf_append_field(CsvBuf *sb, const char *val, size_t len,
                                 const CsvEncodeOpts *o)
{
    if (o->always_quote || csv_needs_quoting(val, len, o->delimiter, o->quote_char)) {
        csvbuf_append_quoted(sb, val, len, o->quote_char, o->escape_char);
    } else {
        csvbuf_append(sb, val, len);
    }
}

/* ================================================================== */
/*  Encode a single value to its CSV string representation             */
/* ================================================================== */

static void csv_encode_field(CsvBuf *sb, EastValue *value, EastType *type,
                              const CsvEncodeOpts *opts)
{
    char numbuf[64];

    /* Handle Option types: unwrap some, output nullString for none */
    if (is_option_type(type)) {
        if (value->kind == EAST_VAL_VARIANT) {
            if (strcmp(value->data.variant.case_name, "none") == 0) {
                csvbuf_append_str(sb, opts->null_string);
                return;
            }
            /* "some" case: unwrap and encode the inner value */
            value = value->data.variant.value;
            type = option_inner_type(type);
        }
    }

    switch (type->kind) {
    case EAST_TYPE_NULL:
        csvbuf_append_str(sb, opts->null_string);
        break;

    case EAST_TYPE_BOOLEAN:
        csvbuf_append_str(sb, value->data.boolean ? "true" : "false");
        break;

    case EAST_TYPE_INTEGER:
        snprintf(numbuf, sizeof(numbuf), "%lld", (long long)value->data.integer);
        csvbuf_append_str(sb, numbuf);
        break;

    case EAST_TYPE_FLOAT: {
        double f = value->data.float64;
        if (f != f) {
            csvbuf_append_str(sb, "NaN");
        } else if (isinf(f)) {
            csvbuf_append_str(sb, f > 0 ? "Infinity" : "-Infinity");
        } else if (f == 0.0 && signbit(f)) {
            csvbuf_append_str(sb, "-0");
        } else {
            east_fmt_double(numbuf, sizeof(numbuf), f);
            csvbuf_append_str(sb, numbuf);
        }
        break;
    }

    case EAST_TYPE_STRING:
        csvbuf_append(sb, value->data.string.data, value->data.string.len);
        break;

    case EAST_TYPE_DATETIME: {
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

        snprintf(numbuf, sizeof(numbuf),
                 "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
                 (int)y, (int)m, (int)d, hour, min, sec, (int)ms);
        csvbuf_append_str(sb, numbuf);
        break;
    }

    case EAST_TYPE_BLOB: {
        csvbuf_append_str(sb, "0x");
        for (size_t i = 0; i < value->data.blob.len; i++) {
            char hex[3];
            snprintf(hex, sizeof(hex), "%02x", value->data.blob.data[i]);
            csvbuf_append_str(sb, hex);
        }
        break;
    }

    default:
        break;
    }
}

/* ================================================================== */
/*  CSV Encoder: Array<Struct> -> CSV string                           */
/* ================================================================== */

char *east_csv_encode(EastValue *array, EastType *type, EastValue *config)
{
    if (!array || !type) return NULL;
    if (type->kind != EAST_TYPE_ARRAY) return NULL;

    EastType *elem_type = type->data.element;
    if (elem_type->kind != EAST_TYPE_STRUCT) return NULL;

    CsvEncodeOpts opts = resolve_encode_opts(config);

    size_t nf = elem_type->data.struct_.num_fields;
    size_t nrows = array->data.array.len;

    CsvBuf sb = csvbuf_new(1024);

    /* Write header row */
    if (opts.include_header) {
        for (size_t i = 0; i < nf; i++) {
            if (i > 0) csvbuf_append_char(&sb, opts.delimiter);
            const char *fname = elem_type->data.struct_.fields[i].name;
            size_t flen = strlen(fname);
            csvbuf_append_field(&sb, fname, flen, &opts);
        }
    }

    /* Write data rows */
    for (size_t r = 0; r < nrows; r++) {
        if (r > 0 || opts.include_header)
            csvbuf_append_str(&sb, opts.newline);

        EastValue *row = array->data.array.items[r];
        if (row->kind != EAST_VAL_STRUCT) continue;

        for (size_t f = 0; f < nf; f++) {
            if (f > 0) csvbuf_append_char(&sb, opts.delimiter);
            const char *fname = elem_type->data.struct_.fields[f].name;
            EastType *ftype = elem_type->data.struct_.fields[f].type;

            EastValue *fval = NULL;
            for (size_t j = 0; j < row->data.struct_.num_fields; j++) {
                if (strcmp(row->data.struct_.field_names[j], fname) == 0) {
                    fval = row->data.struct_.field_values[j];
                    break;
                }
            }

            if (fval) {
                CsvBuf tmp = csvbuf_new(64);
                csv_encode_field(&tmp, fval, ftype, &opts);
                csvbuf_append_field(&sb, tmp.data, tmp.len, &opts);
                free(tmp.data);
            } else {
                csvbuf_append_str(&sb, opts.null_string);
            }
        }
    }

    return sb.data;
}

/* ================================================================== */
/*  CSV Parser helpers                                                 */
/* ================================================================== */

typedef struct {
    char **fields;
    size_t count;
    size_t cap;
} FieldArray;

static FieldArray field_array_new(void)
{
    FieldArray fa;
    fa.cap = 16;
    fa.fields = malloc(fa.cap * sizeof(char *));
    fa.count = 0;
    return fa;
}

static void field_array_push(FieldArray *fa, const char *str, size_t len)
{
    if (fa->count >= fa->cap) {
        fa->cap *= 2;
        fa->fields = realloc(fa->fields, fa->cap * sizeof(char *));
    }
    char *s = malloc(len + 1);
    if (s) {
        memcpy(s, str, len);
        s[len] = '\0';
    }
    fa->fields[fa->count++] = s;
}

static void field_array_free(FieldArray *fa)
{
    for (size_t i = 0; i < fa->count; i++) {
        free(fa->fields[i]);
    }
    free(fa->fields);
}

/* Parse a single CSV row from data at *offset.
 * Returns field array. Sets *offset to position after row.
 * Sets *is_end to true if we've reached end of data. */
static FieldArray csv_parse_row(const char *data, size_t data_len,
                                size_t *offset, bool *is_end,
                                char delim, char quote, char escape)
{
    FieldArray fa = field_array_new();
    CsvBuf field = csvbuf_new(64);
    bool in_quote = false;
    size_t i = *offset;

    while (i < data_len) {
        char c = data[i];

        if (in_quote) {
            if (c == escape && i + 1 < data_len && data[i+1] == quote) {
                csvbuf_append_char(&field, quote);
                i += 2;
            } else if (c == quote) {
                in_quote = false;
                i++;
            } else {
                csvbuf_append_char(&field, c);
                i++;
            }
        } else {
            if (c == quote && field.len == 0) {
                in_quote = true;
                i++;
            } else if (c == delim) {
                field_array_push(&fa, field.data, field.len);
                field.len = 0;
                field.data[0] = '\0';
                i++;
            } else if (c == '\r') {
                field_array_push(&fa, field.data, field.len);
                if (i + 1 < data_len && data[i+1] == '\n') i++;
                i++;
                *offset = i;
                *is_end = false;
                free(field.data);
                return fa;
            } else if (c == '\n') {
                field_array_push(&fa, field.data, field.len);
                i++;
                *offset = i;
                *is_end = false;
                free(field.data);
                return fa;
            } else {
                csvbuf_append_char(&field, c);
                i++;
            }
        }
    }

    /* End of data */
    if (in_quote) {
        /* Unclosed quote - signal error by returning what we have.
         * The caller (builtin) handles the error. */
    }
    field_array_push(&fa, field.data, field.len);
    *offset = i;
    *is_end = true;
    free(field.data);
    return fa;
}

/* Check if a row is empty (all fields are empty strings) */
static bool csv_row_is_empty(FieldArray *fa)
{
    if (fa->count == 0) return true;
    for (size_t i = 0; i < fa->count; i++) {
        if (fa->fields[i] && fa->fields[i][0] != '\0') return false;
    }
    return true;
}

/* ================================================================== */
/*  Parse a single CSV field string into an East value                 */
/* ================================================================== */

static EastValue *csv_parse_field(const char *str, EastType *type,
                                   const CsvDecodeOpts *opts)
{
    if (!str) return east_null();

    /* Apply trim if configured */
    char *trimmed = NULL;
    if (opts->trim_fields) {
        const char *start = str;
        while (*start && isspace((unsigned char)*start)) start++;
        const char *end = str + strlen(str);
        while (end > start && isspace((unsigned char)*(end - 1))) end--;
        size_t tlen = end - start;
        trimmed = malloc(tlen + 1);
        if (trimmed) {
            memcpy(trimmed, start, tlen);
            trimmed[tlen] = '\0';
        }
        str = trimmed;
    }

    /* Handle Option types */
    bool is_opt = is_option_type(type);
    EastType *base_type = is_opt ? option_inner_type(type) : type;

    /* Check for null */
    if (is_null_string(opts, str)) {
        free(trimmed);
        if (is_opt) {
            return east_variant_new("none", east_null(), NULL);
        } else {
            /* Null for required field - return error via NULL */
            return NULL;
        }
    }

    /* Parse based on base type */
    EastValue *parsed = NULL;

    switch (base_type->kind) {
    case EAST_TYPE_NULL:
        parsed = east_null();
        break;

    case EAST_TYPE_BOOLEAN:
        if (strcmp(str, "true") == 0) parsed = east_boolean(true);
        else if (strcmp(str, "false") == 0) parsed = east_boolean(false);
        break;

    case EAST_TYPE_INTEGER: {
        char *end;
        long long val = strtoll(str, &end, 10);
        if (*end == '\0') parsed = east_integer((int64_t)val);
        break;
    }

    case EAST_TYPE_FLOAT: {
        if (strcmp(str, "NaN") == 0) { parsed = east_float(NAN); break; }
        if (strcmp(str, "Infinity") == 0) { parsed = east_float(INFINITY); break; }
        if (strcmp(str, "-Infinity") == 0) { parsed = east_float(-INFINITY); break; }
        if (strcmp(str, "-0") == 0 || strcmp(str, "-0.0") == 0) {
            parsed = east_float(-0.0);
            break;
        }
        char *end;
        double val = strtod(str, &end);
        if (*end == '\0') parsed = east_float(val);
        break;
    }

    case EAST_TYPE_STRING:
        parsed = east_string(str);
        break;

    case EAST_TYPE_DATETIME: {
        int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0, ms = 0;
        size_t slen = strlen(str);

        if (slen >= 19) {
            sscanf(str, "%d-%d-%dT%d:%d:%d",
                   &year, &month, &day, &hour, &min, &sec);
            const char *dot = strchr(str, '.');
            if (dot) {
                sscanf(dot + 1, "%d", &ms);
                size_t digits = 0;
                const char *dp = dot + 1;
                while (*dp >= '0' && *dp <= '9') { digits++; dp++; }
                if (digits > 3) {
                    char msbuf[4];
                    memcpy(msbuf, dot + 1, 3);
                    msbuf[3] = '\0';
                    ms = atoi(msbuf);
                }
            }
        }

        int tz_sign = 1, tz_hour = 0, tz_min = 0;
        for (size_t i = 19; i < slen; i++) {
            if (str[i] == 'Z' || str[i] == 'z') break;
            if (str[i] == '+' || str[i] == '-') {
                tz_sign = (str[i] == '-') ? -1 : 1;
                sscanf(str + i + 1, "%d:%d", &tz_hour, &tz_min);
                break;
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

        parsed = east_datetime(epoch_secs * 1000 + ms);
        break;
    }

    case EAST_TYPE_BLOB: {
        if (strncmp(str, "0x", 2) != 0) break;
        const char *hex = str + 2;
        size_t hlen = strlen(hex);
        if (hlen % 2 != 0) break;

        size_t blen = hlen / 2;
        uint8_t *bdata = malloc(blen);
        if (!bdata) break;

        for (size_t i = 0; i < blen; i++) {
            char byte_str[3] = { hex[i*2], hex[i*2+1], '\0' };
            bdata[i] = (uint8_t)strtoul(byte_str, NULL, 16);
        }

        parsed = east_blob(bdata, blen);
        free(bdata);
        break;
    }

    default:
        parsed = east_null();
        break;
    }

    free(trimmed);

    if (!parsed) return NULL;

    /* Wrap in Option if needed */
    if (is_opt) {
        EastValue *wrapped = east_variant_new("some", parsed, NULL);
        east_value_release(parsed);
        return wrapped;
    }
    return parsed;
}

/* ================================================================== */
/*  CSV Decoder: CSV string -> Array<Struct>                           */
/* ================================================================== */

EastValue *east_csv_decode(const char *csv, EastType *type, EastValue *config)
{
    if (!csv || !type) return NULL;
    if (type->kind != EAST_TYPE_ARRAY) return NULL;

    EastType *elem_type = type->data.element;
    if (elem_type->kind != EAST_TYPE_STRUCT) return NULL;

    CsvDecodeOpts opts = resolve_decode_opts(config);

    size_t nf = elem_type->data.struct_.num_fields;
    size_t data_len = strlen(csv);
    size_t offset = 0;

    /* Skip UTF-8 BOM */
    if (data_len >= 3 && (unsigned char)csv[0] == 0xEF &&
        (unsigned char)csv[1] == 0xBB && (unsigned char)csv[2] == 0xBF) {
        offset = 3;
    }

    /* Build mapping: for each struct field, find its column index */
    int *col_indices = malloc(nf * sizeof(int));
    if (!col_indices) {
        decode_opts_free(&opts);
        return NULL;
    }

    bool is_end = false;

    if (opts.has_header) {
        /* Parse header row */
        FieldArray header = csv_parse_row(csv, data_len, &offset, &is_end,
                                           opts.delimiter, opts.quote_char, opts.escape_char);

        /* Get optional columnMapping: Dict<String,String> mapping
         * CSV header names -> struct field names */
        EastValue *col_mapping = config_get_dict(config, "columnMapping");

        for (size_t f = 0; f < nf; f++) {
            col_indices[f] = -1;
            const char *fname = elem_type->data.struct_.fields[f].name;
            for (size_t h = 0; h < header.count; h++) {
                /* Apply columnMapping: if the header has a mapping,
                 * use the mapped name for comparison */
                const char *mapped = dict_lookup_string(col_mapping, header.fields[h]);
                const char *hname = mapped ? mapped : header.fields[h];
                if (strcmp(hname, fname) == 0) {
                    col_indices[f] = (int)h;
                    break;
                }
            }
        }

        /* Check for missing required columns */
        for (size_t f = 0; f < nf; f++) {
            if (col_indices[f] < 0) {
                EastType *ftype = elem_type->data.struct_.fields[f].type;
                if (!is_option_type(ftype)) {
                    /* Missing required column - error */
                    field_array_free(&header);
                    free(col_indices);
                    decode_opts_free(&opts);
                    return NULL;
                }
            }
        }

        /* Strict mode: check for extra columns */
        if (opts.strict) {
            for (size_t h = 0; h < header.count; h++) {
                const char *mapped = dict_lookup_string(col_mapping, header.fields[h]);
                const char *hname = mapped ? mapped : header.fields[h];
                bool found = false;
                for (size_t f = 0; f < nf; f++) {
                    if (strcmp(hname, elem_type->data.struct_.fields[f].name) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    field_array_free(&header);
                    free(col_indices);
                    decode_opts_free(&opts);
                    return NULL;
                }
            }
        }

        field_array_free(&header);
    } else {
        /* No header: columns map by position to struct field order */
        for (size_t f = 0; f < nf; f++) {
            col_indices[f] = (int)f;
        }
    }

    /* Parse data rows */
    EastValue *result = east_array_new(elem_type);
    if (!result) {
        free(col_indices);
        decode_opts_free(&opts);
        return NULL;
    }

    while (offset < data_len && !is_end) {
        FieldArray row = csv_parse_row(csv, data_len, &offset, &is_end,
                                        opts.delimiter, opts.quote_char, opts.escape_char);

        /* Skip empty rows (unless skipEmptyLines is false) */
        if (opts.skip_empty_lines && csv_row_is_empty(&row)) {
            field_array_free(&row);
            if (is_end) break;
            continue;
        }

        /* Build struct from row */
        const char **names = malloc(nf * sizeof(char *));
        EastValue **values = malloc(nf * sizeof(EastValue *));
        if (!names || !values) {
            free(names); free(values);
            field_array_free(&row);
            break;
        }

        bool row_ok = true;
        for (size_t f = 0; f < nf; f++) {
            names[f] = elem_type->data.struct_.fields[f].name;
            EastType *ftype = elem_type->data.struct_.fields[f].type;
            int ci = col_indices[f];

            if (ci >= 0 && (size_t)ci < row.count) {
                values[f] = csv_parse_field(row.fields[ci], ftype, &opts);
                if (!values[f]) {
                    /* Parse error - null for required field or type error */
                    row_ok = false;
                    /* Clean up already-parsed values */
                    for (size_t j = 0; j < f; j++)
                        east_value_release(values[j]);
                    break;
                }
            } else {
                /* Column missing from this row */
                if (is_option_type(ftype)) {
                    values[f] = east_variant_new("none", east_null(), NULL);
                } else {
                    values[f] = east_null();
                }
            }
        }

        if (row_ok) {
            EastValue *struct_val = east_struct_new(names, values, nf, elem_type);
            if (struct_val) {
                east_array_push(result, struct_val);
                east_value_release(struct_val);
            }
            for (size_t f = 0; f < nf; f++) {
                east_value_release(values[f]);
            }
        }

        free(names);
        free(values);
        field_array_free(&row);

        if (!row_ok) {
            east_value_release(result);
            free(col_indices);
            decode_opts_free(&opts);
            return NULL;
        }

        if (is_end) break;
    }

    free(col_indices);
    decode_opts_free(&opts);
    return result;
}
