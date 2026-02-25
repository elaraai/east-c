/*
 * String builtin functions.
 *
 * All string operations use Unicode codepoint semantics to match JavaScript:
 * - StringLength returns codepoint count, not byte count
 * - StringIndexOf returns codepoint index, not byte offset
 * - StringSubstring takes codepoint indices
 * - StringSplit with empty delimiter splits into codepoints
 * - Regex uses PCRE2 for JavaScript-compatible pattern matching
 */
#include "east/builtins.h"
#include "east/values.h"
#include "east/serialization.h"
#include <ctype.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include <pcre2.h>

/* ------------------------------------------------------------------ */
/*  UTF-8 helpers                                                      */
/* ------------------------------------------------------------------ */

/* Return the byte length of a UTF-8 character starting at *p. */
static inline size_t utf8_char_len(const unsigned char *p) {
    if (*p < 0x80) return 1;
    if ((*p & 0xE0) == 0xC0) return 2;
    if ((*p & 0xF0) == 0xE0) return 3;
    if ((*p & 0xF8) == 0xF0) return 4;
    return 1; /* invalid byte — advance 1 */
}

/* Count the number of Unicode codepoints in a UTF-8 string. */
static size_t utf8_codepoint_count(const char *s, size_t byte_len) {
    size_t count = 0;
    const unsigned char *p = (const unsigned char *)s;
    const unsigned char *end = p + byte_len;
    while (p < end) {
        p += utf8_char_len(p);
        count++;
    }
    return count;
}

/* Convert a codepoint index to a byte offset.
 * Returns byte_len if cp_index >= total codepoints. */
static size_t utf8_cp_to_byte(const char *s, size_t byte_len, size_t cp_index) {
    const unsigned char *p = (const unsigned char *)s;
    const unsigned char *end = p + byte_len;
    size_t cp = 0;
    while (p < end && cp < cp_index) {
        p += utf8_char_len(p);
        cp++;
    }
    return (size_t)(p - (const unsigned char *)s);
}

/* Convert a byte offset to a codepoint index. */
static size_t utf8_byte_to_cp(const char *s, size_t byte_offset) {
    const unsigned char *p = (const unsigned char *)s;
    const unsigned char *target = p + byte_offset;
    size_t cp = 0;
    while (p < target) {
        p += utf8_char_len(p);
        cp++;
    }
    return cp;
}

/* ------------------------------------------------------------------ */
/*  Basic string operations                                            */
/* ------------------------------------------------------------------ */

static EastValue *string_concat(EastValue **args, size_t n) {
    (void)n;
    const char *a = args[0]->data.string.data;
    size_t alen = args[0]->data.string.len;
    const char *b = args[1]->data.string.data;
    size_t blen = args[1]->data.string.len;
    size_t total = alen + blen;
    char *buf = malloc(total + 1);
    if (!buf) return east_string("");
    memcpy(buf, a, alen);
    memcpy(buf + alen, b, blen);
    buf[total] = '\0';
    EastValue *result = east_string_len(buf, total);
    free(buf);
    return result;
}

static EastValue *string_repeat(EastValue **args, size_t n) {
    (void)n;
    const char *s = args[0]->data.string.data;
    size_t slen = args[0]->data.string.len;
    int64_t count = args[1]->data.integer;
    if (count <= 0 || slen == 0) return east_string("");
    size_t total = slen * (size_t)count;
    char *buf = malloc(total + 1);
    if (!buf) return east_string("");
    for (int64_t i = 0; i < count; i++) {
        memcpy(buf + i * slen, s, slen);
    }
    buf[total] = '\0';
    EastValue *result = east_string_len(buf, total);
    free(buf);
    return result;
}

/* StringLength — returns Unicode codepoint count (like JS for...of) */
static EastValue *string_length(EastValue **args, size_t n) {
    (void)n;
    size_t cp_count = utf8_codepoint_count(args[0]->data.string.data,
                                            args[0]->data.string.len);
    return east_integer((int64_t)cp_count);
}

/* StringSubstring — takes codepoint indices (like JS) */
static EastValue *string_substring(EastValue **args, size_t n) {
    (void)n;
    const char *s = args[0]->data.string.data;
    size_t byte_len = args[0]->data.string.len;
    int64_t from = args[1]->data.integer;
    int64_t to = args[2]->data.integer;

    /* Forgiving semantics like JS */
    if (from < 0) from = 0;
    if (to < 0) to = 0;
    if (from > to) to = from;

    size_t total_cp = utf8_codepoint_count(s, byte_len);
    if ((size_t)from >= total_cp) return east_string("");
    if ((size_t)to > total_cp) to = (int64_t)total_cp;

    size_t byte_from = utf8_cp_to_byte(s, byte_len, (size_t)from);
    size_t byte_to = utf8_cp_to_byte(s, byte_len, (size_t)to);
    return east_string_len(s + byte_from, byte_to - byte_from);
}

/* StringIndexOf — returns codepoint index (like JS) */
static EastValue *string_index_of(EastValue **args, size_t n) {
    (void)n;
    const char *s = args[0]->data.string.data;
    const char *sub = args[1]->data.string.data;
    const char *found = strstr(s, sub);
    if (!found) return east_integer(-1);
    /* Convert byte offset to codepoint index */
    size_t byte_offset = (size_t)(found - s);
    size_t cp_index = utf8_byte_to_cp(s, byte_offset);
    return east_integer((int64_t)cp_index);
}

/* StringSplit — splits into codepoints when delimiter is empty */
static EastValue *string_split(EastValue **args, size_t n) {
    (void)n;
    const char *s = args[0]->data.string.data;
    size_t slen = args[0]->data.string.len;
    const char *delim = args[1]->data.string.data;
    size_t dlen = args[1]->data.string.len;

    EastValue *arr = east_array_new(&east_string_type);

    if (dlen == 0) {
        /* Split into individual codepoints (like JS [...str]) */
        if (slen == 0) {
            EastValue *empty = east_string("");
            east_array_push(arr, empty);
            east_value_release(empty);
        } else {
            const unsigned char *p = (const unsigned char *)s;
            const unsigned char *end = p + slen;
            while (p < end) {
                size_t cl = utf8_char_len(p);
                EastValue *ch = east_string_len((const char *)p, cl);
                east_array_push(arr, ch);
                east_value_release(ch);
                p += cl;
            }
        }
    } else {
        const char *pos = s;
        const char *end = s + slen;
        while (pos <= end) {
            const char *found = strstr(pos, delim);
            if (!found || found > end) {
                EastValue *part = east_string_len(pos, (size_t)(end - pos));
                east_array_push(arr, part);
                east_value_release(part);
                break;
            }
            EastValue *part = east_string_len(pos, (size_t)(found - pos));
            east_array_push(arr, part);
            east_value_release(part);
            pos = found + dlen;
        }
    }
    return arr;
}

static EastValue *string_trim(EastValue **args, size_t n) {
    (void)n;
    const char *s = args[0]->data.string.data;
    size_t len = args[0]->data.string.len;
    size_t start = 0;
    while (start < len && isspace((unsigned char)s[start])) start++;
    size_t end = len;
    while (end > start && isspace((unsigned char)s[end - 1])) end--;
    return east_string_len(s + start, end - start);
}

static EastValue *string_trim_start(EastValue **args, size_t n) {
    (void)n;
    const char *s = args[0]->data.string.data;
    size_t len = args[0]->data.string.len;
    size_t start = 0;
    while (start < len && isspace((unsigned char)s[start])) start++;
    return east_string_len(s + start, len - start);
}

static EastValue *string_trim_end(EastValue **args, size_t n) {
    (void)n;
    const char *s = args[0]->data.string.data;
    size_t len = args[0]->data.string.len;
    while (len > 0 && isspace((unsigned char)s[len - 1])) len--;
    return east_string_len(s, len);
}

/* Decode one UTF-8 codepoint, returning the codepoint and advancing *advance. */
static uint32_t utf8_decode_cp(const unsigned char *p, size_t *advance) {
    if (*p < 0x80)         { *advance = 1; return *p; }
    if ((*p & 0xE0) == 0xC0) { *advance = 2; return ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F); }
    if ((*p & 0xF0) == 0xE0) { *advance = 3; return ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); }
    if ((*p & 0xF8) == 0xF0) { *advance = 4; return ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F); }
    *advance = 1; return 0xFFFD;
}

/* Encode one codepoint as UTF-8, return bytes written. */
static size_t utf8_encode_cp(uint32_t cp, char *buf) {
    if (cp < 0x80)    { buf[0] = (char)cp; return 1; }
    if (cp < 0x800)   { buf[0] = (char)(0xC0 | (cp >> 6)); buf[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { buf[0] = (char)(0xE0 | (cp >> 12)); buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    buf[0] = (char)(0xF0 | (cp >> 18)); buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

static int locale_init_done = 0;
static void ensure_utf8_locale(void) {
    if (!locale_init_done) {
        setlocale(LC_CTYPE, "C.UTF-8");
        locale_init_done = 1;
    }
}

/* StringLowerCase / StringUpperCase — full Unicode case mapping via towlower/towupper. */
static EastValue *string_lower_case(EastValue **args, size_t n) {
    (void)n;
    ensure_utf8_locale();
    const unsigned char *s = (const unsigned char *)args[0]->data.string.data;
    size_t len = args[0]->data.string.len;
    char *buf = malloc(len * 4 + 1); /* worst case expansion */
    if (!buf) return east_string("");
    size_t out = 0;
    const unsigned char *end = s + len;
    while (s < end) {
        size_t advance;
        uint32_t cp = utf8_decode_cp(s, &advance);
        uint32_t lc = (uint32_t)towlower((wint_t)cp);
        out += utf8_encode_cp(lc, buf + out);
        s += advance;
    }
    buf[out] = '\0';
    EastValue *result = east_string_len(buf, out);
    free(buf);
    return result;
}

static EastValue *string_upper_case(EastValue **args, size_t n) {
    (void)n;
    ensure_utf8_locale();
    const unsigned char *s = (const unsigned char *)args[0]->data.string.data;
    size_t len = args[0]->data.string.len;
    char *buf = malloc(len * 4 + 1);
    if (!buf) return east_string("");
    size_t out = 0;
    const unsigned char *end = s + len;
    while (s < end) {
        size_t advance;
        uint32_t cp = utf8_decode_cp(s, &advance);
        uint32_t uc = (uint32_t)towupper((wint_t)cp);
        out += utf8_encode_cp(uc, buf + out);
        s += advance;
    }
    buf[out] = '\0';
    EastValue *result = east_string_len(buf, out);
    free(buf);
    return result;
}

/* StringReplace — replaces all occurrences (like JS replaceAll with string) */
static EastValue *string_replace(EastValue **args, size_t n) {
    (void)n;
    const char *s = args[0]->data.string.data;
    size_t slen = args[0]->data.string.len;
    const char *old_str = args[1]->data.string.data;
    size_t old_len = args[1]->data.string.len;
    const char *new_str = args[2]->data.string.data;
    size_t new_len = args[2]->data.string.len;

    if (old_len == 0) {
        /* JS replaceAll("", x) inserts x before each char and at the end */
        size_t cp_count = utf8_codepoint_count(s, slen);
        size_t result_len = slen + new_len * (cp_count + 1);
        char *buf = malloc(result_len + 1);
        if (!buf) return east_string("");
        char *dst = buf;
        const unsigned char *p = (const unsigned char *)s;
        const unsigned char *end = p + slen;
        /* Insert replacement before each codepoint */
        while (p < end) {
            memcpy(dst, new_str, new_len);
            dst += new_len;
            size_t cl = utf8_char_len(p);
            memcpy(dst, p, cl);
            dst += cl;
            p += cl;
        }
        /* Insert replacement at end */
        memcpy(dst, new_str, new_len);
        dst += new_len;
        *dst = '\0';
        EastValue *result = east_string_len(buf, (size_t)(dst - buf));
        free(buf);
        return result;
    }

    /* Count occurrences */
    size_t count = 0;
    const char *p = s;
    while ((p = strstr(p, old_str)) != NULL) {
        count++;
        p += old_len;
    }
    if (count == 0) return east_string_len(s, slen);

    size_t result_len = slen + count * new_len - count * old_len;
    char *buf = malloc(result_len + 1);
    if (!buf) return east_string("");

    char *dst = buf;
    const char *src = s;
    while ((p = strstr(src, old_str)) != NULL) {
        size_t prefix = (size_t)(p - src);
        memcpy(dst, src, prefix);
        dst += prefix;
        memcpy(dst, new_str, new_len);
        dst += new_len;
        src = p + old_len;
    }
    size_t remaining = slen - (size_t)(src - s);
    memcpy(dst, src, remaining);
    dst += remaining;
    *dst = '\0';

    EastValue *result = east_string_len(buf, result_len);
    free(buf);
    return result;
}

static EastValue *string_starts_with(EastValue **args, size_t n) {
    (void)n;
    const char *s = args[0]->data.string.data;
    size_t slen = args[0]->data.string.len;
    const char *prefix = args[1]->data.string.data;
    size_t plen = args[1]->data.string.len;
    if (plen > slen) return east_boolean(false);
    return east_boolean(memcmp(s, prefix, plen) == 0);
}

static EastValue *string_ends_with(EastValue **args, size_t n) {
    (void)n;
    const char *s = args[0]->data.string.data;
    size_t slen = args[0]->data.string.len;
    const char *suffix = args[1]->data.string.data;
    size_t sflen = args[1]->data.string.len;
    if (sflen > slen) return east_boolean(false);
    return east_boolean(memcmp(s + slen - sflen, suffix, sflen) == 0);
}

static EastValue *string_contains(EastValue **args, size_t n) {
    (void)n;
    const char *s = args[0]->data.string.data;
    const char *sub = args[1]->data.string.data;
    return east_boolean(strstr(s, sub) != NULL);
}

/* ------------------------------------------------------------------ */
/*  PCRE2 regex operations (JavaScript-compatible)                     */
/* ------------------------------------------------------------------ */

/* Convert JS flags string to PCRE2 options */
static uint32_t js_flags_to_pcre2(const char *flags) {
    uint32_t options = PCRE2_UTF | PCRE2_UCP;
    for (const char *p = flags; *p; p++) {
        switch (*p) {
            case 'i': options |= PCRE2_CASELESS; break;
            case 'm': options |= PCRE2_MULTILINE; break;
            case 's': options |= PCRE2_DOTALL; break;
            case 'g': break; /* handled separately */
            default: break;
        }
    }
    return options;
}

/* Compile a PCRE2 regex from pattern and flags. Returns NULL on error. */
static pcre2_code *compile_regex(const char *pattern, const char *flags) {
    int errorcode;
    PCRE2_SIZE erroroffset;
    uint32_t options = js_flags_to_pcre2(flags);

    pcre2_code *re = pcre2_compile(
        (PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
        options, &errorcode, &erroroffset, NULL);

    if (!re) {
        PCRE2_UCHAR errbuf[256];
        pcre2_get_error_message(errorcode, errbuf, sizeof(errbuf));
        char msg[512];
        snprintf(msg, sizeof(msg), "Invalid regular expression: %s", (char *)errbuf);
        east_builtin_error(msg);
    }
    return re;
}

/* RegexContains — like JS regex.test(text) */
static EastValue *regex_contains(EastValue **args, size_t n) {
    (void)n;
    const char *text = args[0]->data.string.data;
    size_t text_len = args[0]->data.string.len;
    const char *pattern = args[1]->data.string.data;
    const char *flags = args[2]->data.string.data;

    pcre2_code *re = compile_regex(pattern, flags);
    if (!re) return NULL;

    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    int rc = pcre2_match(re, (PCRE2_SPTR)text, text_len, 0, 0, md, NULL);
    pcre2_match_data_free(md);
    pcre2_code_free(re);

    return east_boolean(rc >= 0);
}

/* RegexIndexOf — returns codepoint index of first match, -1 if none */
static EastValue *regex_index_of(EastValue **args, size_t n) {
    (void)n;
    const char *text = args[0]->data.string.data;
    size_t text_len = args[0]->data.string.len;
    const char *pattern = args[1]->data.string.data;
    const char *flags = args[2]->data.string.data;

    pcre2_code *re = compile_regex(pattern, flags);
    if (!re) return NULL;

    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    int rc = pcre2_match(re, (PCRE2_SPTR)text, text_len, 0, 0, md, NULL);

    int64_t result = -1;
    if (rc >= 0) {
        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(md);
        size_t byte_offset = ovector[0];
        /* Convert byte offset to codepoint index */
        result = (int64_t)utf8_byte_to_cp(text, byte_offset);
    }

    pcre2_match_data_free(md);
    pcre2_code_free(re);
    return east_integer(result);
}

/* Dynamic string buffer for regex replace */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} DynBuf;

static void dynbuf_init(DynBuf *b) { b->data = NULL; b->len = 0; b->cap = 0; }

static void dynbuf_append(DynBuf *b, const char *s, size_t n) {
    if (b->len + n >= b->cap) {
        size_t new_cap = (b->cap == 0) ? 256 : b->cap * 2;
        while (new_cap < b->len + n + 1) new_cap *= 2;
        b->data = realloc(b->data, new_cap);
        b->cap = new_cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

/* Process replacement string with $-substitutions (JS semantics).
 * md is the match data, text is the subject string. */
static void apply_replacement(DynBuf *buf, const char *replacement, size_t rlen,
                               const char *text, pcre2_match_data *md,
                               pcre2_code *re) {
    PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(md);
    uint32_t capture_count;
    pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &capture_count);

    size_t i = 0;
    while (i < rlen) {
        if (replacement[i] == '$' && i + 1 < rlen) {
            char next = replacement[i + 1];
            if (next == '$') {
                /* $$ -> literal $ */
                dynbuf_append(buf, "$", 1);
                i += 2;
            } else if (next >= '1' && next <= '9') {
                /* $N or $NN - numbered capture group */
                i += 1;
                int group = 0;
                while (i < rlen && replacement[i] >= '0' && replacement[i] <= '9') {
                    int new_group = group * 10 + (replacement[i] - '0');
                    /* Only consume if the group number is valid */
                    if ((uint32_t)new_group > capture_count) break;
                    group = new_group;
                    i++;
                }
                if (group > 0 && (uint32_t)group <= capture_count) {
                    PCRE2_SIZE start = ovector[2 * group];
                    PCRE2_SIZE end = ovector[2 * group + 1];
                    if (start != PCRE2_UNSET) {
                        dynbuf_append(buf, text + start, end - start);
                    }
                } else {
                    /* Invalid group reference - output literally */
                    dynbuf_append(buf, "$", 1);
                }
            } else if (next == '<') {
                /* $<name> - named capture group */
                i += 2; /* skip $< */
                const char *name_start = replacement + i;
                while (i < rlen && replacement[i] != '>') i++;
                if (i < rlen) {
                    size_t name_len = (size_t)(replacement + i - name_start);
                    char *name = malloc(name_len + 1);
                    memcpy(name, name_start, name_len);
                    name[name_len] = '\0';
                    int group_num = pcre2_substring_number_from_name(re, (PCRE2_SPTR)name);
                    if (group_num > 0 && (uint32_t)group_num <= capture_count) {
                        PCRE2_SIZE start = ovector[2 * group_num];
                        PCRE2_SIZE end = ovector[2 * group_num + 1];
                        if (start != PCRE2_UNSET) {
                            dynbuf_append(buf, text + start, end - start);
                        }
                    }
                    free(name);
                    i++; /* skip > */
                }
            } else {
                /* Unsupported $ sequence - output literally */
                dynbuf_append(buf, "$", 1);
                i++;
            }
        } else {
            dynbuf_append(buf, replacement + i, 1);
            i++;
        }
    }
}

/* RegexReplace — replaces all matches (like JS replaceAll with regex) */
static EastValue *regex_replace(EastValue **args, size_t n) {
    (void)n;
    const char *text = args[0]->data.string.data;
    size_t text_len = args[0]->data.string.len;
    const char *pattern = args[1]->data.string.data;
    const char *flags = args[2]->data.string.data;
    const char *replacement = args[3]->data.string.data;
    size_t rlen = args[3]->data.string.len;

    /* Validate replacement string (match JS's strict validation) */
    for (size_t i = 0; i < rlen; i++) {
        if (replacement[i] == '$') {
            i++;
            if (i >= rlen) {
                east_builtin_error("invalid regex replacement string: unescaped $ at end of string");
                return NULL;
            }
            char c = replacement[i];
            if (c == '$') {
                /* OK - escaped dollar */
            } else if (c >= '1' && c <= '9') {
                /* OK - consume additional digits */
                while (i + 1 < rlen && replacement[i + 1] >= '0' && replacement[i + 1] <= '9') i++;
            } else if (c == '<') {
                /* Scan for closing > */
                i++;
                size_t name_start = i;
                while (i < rlen && replacement[i] != '>') {
                    char ch = replacement[i];
                    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
                          (ch >= 'A' && ch <= 'Z') || ch == '_')) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "invalid regex replacement string: invalid character \"%c\" in group name in $<...>", ch);
                        east_builtin_error(msg);
                        return NULL;
                    }
                    i++;
                }
                if (i >= rlen) {
                    east_builtin_error("invalid regex replacement string: unterminated group name in $<...>");
                    return NULL;
                }
                if (i == name_start) {
                    east_builtin_error("invalid regex replacement string: empty group name in $<>");
                    return NULL;
                }
            } else {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "invalid regex replacement string: unescaped $ at $%c", c);
                east_builtin_error(msg);
                return NULL;
            }
        }
    }

    pcre2_code *re = compile_regex(pattern, flags);
    if (!re) return NULL;

    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    DynBuf buf;
    dynbuf_init(&buf);

    PCRE2_SIZE offset = 0;
    while (offset <= text_len) {
        int rc = pcre2_match(re, (PCRE2_SPTR)text, text_len, offset, 0, md, NULL);
        if (rc < 0) break;

        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(md);
        PCRE2_SIZE match_start = ovector[0];
        PCRE2_SIZE match_end = ovector[1];

        /* Append text before match */
        if (match_start > offset) {
            dynbuf_append(&buf, text + offset, match_start - offset);
        }

        /* Apply replacement with $-substitutions */
        apply_replacement(&buf, replacement, rlen, text, md, re);

        /* Advance past match; handle zero-length matches */
        if (match_end == match_start) {
            if (match_start < text_len) {
                size_t cl = utf8_char_len((const unsigned char *)(text + match_start));
                dynbuf_append(&buf, text + match_start, cl);
                offset = match_start + cl;
            } else {
                offset = text_len; /* no remaining text to append */
                break;
            }
        } else {
            offset = match_end;
        }
    }

    /* Append remaining text */
    if (offset < text_len) {
        dynbuf_append(&buf, text + offset, text_len - offset);
    }

    pcre2_match_data_free(md);
    pcre2_code_free(re);

    EastValue *result;
    if (buf.data) {
        result = east_string_len(buf.data, buf.len);
        free(buf.data);
    } else {
        result = east_string("");
    }
    return result;
}

/* ------------------------------------------------------------------ */
/*  Print / Parse / JSON / Error builtins                              */
/* ------------------------------------------------------------------ */

/* Print: value -> East text format string (type-parameterized) */
static _Thread_local EastType *s_print_east_type = NULL;
static EastValue *string_print_east_impl(EastValue **args, size_t n) {
    (void)n;
    char *text = east_print_value(args[0], s_print_east_type);
    if (!text) return east_string("");
    EastValue *result = east_string(text);
    free(text);
    return result;
}

/* Parse: East text format string -> value (type-parameterized) */
static _Thread_local EastType *s_parse_east_type = NULL;
static EastValue *string_parse_east_impl(EastValue **args, size_t n) {
    (void)n;
    const char *text = args[0]->data.string.data;
    char *error_msg = NULL;
    EastValue *result = east_parse_value_with_error(text, s_parse_east_type, &error_msg);
    if (!result) {
        east_builtin_error(error_msg ? error_msg : "Failed to parse value");
        free(error_msg);
        return NULL;
    }
    return result;
}

/* StringPrintJSON: value -> JSON string */
static _Thread_local EastType *s_print_json_type = NULL;
static EastValue *string_print_json_impl(EastValue **args, size_t n) {
    (void)n;
    char *json = east_json_encode(args[0], s_print_json_type);
    if (!json) return east_string("null");
    EastValue *result = east_string(json);
    free(json);
    return result;
}

/* StringParseJSON: JSON string -> value */
static _Thread_local EastType *s_parse_json_type = NULL;
static EastValue *string_parse_json_impl(EastValue **args, size_t n) {
    (void)n;
    const char *json_str = args[0]->data.string.data;
    char *error_msg = NULL;
    EastValue *result = east_json_decode_with_error(json_str, s_parse_json_type, &error_msg);
    if (!result) {
        east_builtin_error(error_msg ? error_msg : "Failed to parse JSON");
        free(error_msg);
        return NULL;
    }
    return result;
}

static EastValue *string_print_error(EastValue **args, size_t n) {
    (void)n;
    /* args: message (string), stack (array of structs) */
    const char *message = args[0]->data.string.data;
    size_t msg_len = args[0]->data.string.len;

    /* Start with "Error: <message>" */
    size_t buf_size = msg_len + 256;
    char *buf = malloc(buf_size);
    if (!buf) return east_string("Error: (allocation failure)");

    int written = snprintf(buf, buf_size, "Error: %s", message);
    (void)written;

    EastValue *result = east_string(buf);
    free(buf);
    return result;
}

/* ------------------------------------------------------------------ */
/*  Factory functions                                                  */
/* ------------------------------------------------------------------ */

static BuiltinImpl string_concat_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return string_concat; }
static BuiltinImpl string_repeat_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return string_repeat; }
static BuiltinImpl string_length_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return string_length; }
static BuiltinImpl string_substring_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return string_substring; }
static BuiltinImpl string_index_of_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return string_index_of; }
static BuiltinImpl string_split_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return string_split; }
static BuiltinImpl string_trim_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return string_trim; }
static BuiltinImpl string_trim_start_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return string_trim_start; }
static BuiltinImpl string_trim_end_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return string_trim_end; }
static BuiltinImpl string_lower_case_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return string_lower_case; }
static BuiltinImpl string_upper_case_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return string_upper_case; }
static BuiltinImpl string_replace_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return string_replace; }
static BuiltinImpl regex_contains_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return regex_contains; }
static BuiltinImpl regex_index_of_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return regex_index_of; }
static BuiltinImpl regex_replace_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return regex_replace; }
static BuiltinImpl string_starts_with_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return string_starts_with; }
static BuiltinImpl string_ends_with_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return string_ends_with; }
static BuiltinImpl string_contains_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return string_contains; }
static BuiltinImpl print_factory(EastType **tp, size_t ntp) {
    if (ntp > 0 && tp[0]) s_print_east_type = tp[0];
    return string_print_east_impl;
}
static BuiltinImpl parse_factory(EastType **tp, size_t ntp) {
    if (ntp > 0 && tp[0]) s_parse_east_type = tp[0];
    return string_parse_east_impl;
}
static BuiltinImpl string_print_json_factory(EastType **tp, size_t ntp) {
    if (ntp > 0 && tp[0]) s_print_json_type = tp[0];
    return string_print_json_impl;
}
static BuiltinImpl string_parse_json_factory(EastType **tp, size_t ntp) {
    if (ntp > 0 && tp[0]) s_parse_json_type = tp[0];
    return string_parse_json_impl;
}
static BuiltinImpl string_print_error_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return string_print_error; }

/* ------------------------------------------------------------------ */
/*  Registration                                                       */
/* ------------------------------------------------------------------ */

void east_register_string_builtins(BuiltinRegistry *reg) {
    builtin_registry_register(reg, "StringConcat", string_concat_factory);
    builtin_registry_register(reg, "StringRepeat", string_repeat_factory);
    builtin_registry_register(reg, "StringLength", string_length_factory);
    builtin_registry_register(reg, "StringSubstring", string_substring_factory);
    builtin_registry_register(reg, "StringIndexOf", string_index_of_factory);
    builtin_registry_register(reg, "StringSplit", string_split_factory);
    builtin_registry_register(reg, "StringTrim", string_trim_factory);
    builtin_registry_register(reg, "StringTrimStart", string_trim_start_factory);
    builtin_registry_register(reg, "StringTrimEnd", string_trim_end_factory);
    builtin_registry_register(reg, "StringLowerCase", string_lower_case_factory);
    builtin_registry_register(reg, "StringUpperCase", string_upper_case_factory);
    builtin_registry_register(reg, "StringReplace", string_replace_factory);
    builtin_registry_register(reg, "RegexContains", regex_contains_factory);
    builtin_registry_register(reg, "RegexIndexOf", regex_index_of_factory);
    builtin_registry_register(reg, "RegexReplace", regex_replace_factory);
    builtin_registry_register(reg, "StringStartsWith", string_starts_with_factory);
    builtin_registry_register(reg, "StringEndsWith", string_ends_with_factory);
    builtin_registry_register(reg, "StringContains", string_contains_factory);
    builtin_registry_register(reg, "Print", print_factory);
    builtin_registry_register(reg, "Parse", parse_factory);
    builtin_registry_register(reg, "StringPrintJSON", string_print_json_factory);
    builtin_registry_register(reg, "StringParseJSON", string_parse_json_factory);
    builtin_registry_register(reg, "StringPrintError", string_print_error_factory);
}
