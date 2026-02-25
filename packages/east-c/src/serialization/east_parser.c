/*
 * Parser for East text format.
 *
 * Type-directed parser: the target type guides how text is parsed.
 * Uses the tokenizer defined in east_tokenizer.c.
 *
 * east_parse_value(text, type) -> EastValue*
 * east_parse_type(text) -> EastType*
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
/*  Token types (must match east_tokenizer.c)                          */
/* ================================================================== */

typedef enum {
    TOK_STRING,
    TOK_INTEGER,
    TOK_FLOAT,
    TOK_TRUE,
    TOK_FALSE,
    TOK_NULL_TOK,
    TOK_DOT,
    TOK_COLON,
    TOK_COMMA,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_EQUALS,
    TOK_AMPERSAND,
    TOK_PIPE,
    TOK_HEX,
    TOK_DATETIME_LIT,
    TOK_IDENTIFIER,
    TOK_VARIANT_TAG,
    TOK_BACKREF,
    TOK_EOF_TOK,
    TOK_ERROR,
} EastTokenType2;  /* suffix to avoid conflict when compiled together */

typedef struct {
    EastTokenType2 type;
    char *text;
    size_t text_len;
    int64_t int_val;
    double float_val;
    int line;
    int column;
} Token2;

typedef struct {
    Token2 *tokens;
    size_t count;
    size_t cap;
} TokenArr2;

typedef struct {
    TokenArr2 ta;
    size_t pos;
} TokStream2;

/* ================================================================== */
/*  Inline tokenizer (self-contained for this compilation unit)        */
/* ================================================================== */

static TokenArr2 ta2_new(void)
{
    TokenArr2 ta;
    ta.cap = 64;
    ta.tokens = calloc(ta.cap, sizeof(Token2));
    ta.count = 0;
    return ta;
}

static void ta2_push(TokenArr2 *ta, Token2 tok)
{
    if (ta->count >= ta->cap) {
        ta->cap *= 2;
        ta->tokens = realloc(ta->tokens, ta->cap * sizeof(Token2));
    }
    ta->tokens[ta->count++] = tok;
}

static void ta2_free(TokenArr2 *ta)
{
    for (size_t i = 0; i < ta->count; i++) {
        free(ta->tokens[i].text);
    }
    free(ta->tokens);
}

/* Simplified re-implementation of tokenizer for parser self-containment */
static TokenArr2 tokenize2(const char *text)
{
    TokenArr2 result = ta2_new();
    size_t len = strlen(text);
    size_t pos = 0;
    int line = 1, col = 1;

    #define ADV2() do { \
        if (pos < len) { \
            if (text[pos] == '\n') { line++; col = 1; } else { col++; } \
            pos++; \
        } \
    } while(0)

    #define CUR2() (pos < len ? text[pos] : '\0')
    #define PEEK2(off) ((pos + (off)) < len ? text[pos + (off)] : '\0')

    while (1) {
        /* Skip whitespace and comments */
        while (pos < len) {
            char c = text[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ADV2();
                continue;
            }
            if (c == '#') {
                while (pos < len && text[pos] != '\n') ADV2();
                continue;
            }
            break;
        }

        if (pos >= len) {
            Token2 eof = {0}; eof.type = TOK_EOF_TOK; eof.line = line; eof.column = col;
            ta2_push(&result, eof);
            break;
        }

        char c = CUR2();
        int sl = line, sc = col;

        if (c == '[') { ADV2(); Token2 t = {0}; t.type = TOK_LBRACKET; t.line = sl; t.column = sc; ta2_push(&result, t); }
        else if (c == ']') { ADV2(); Token2 t = {0}; t.type = TOK_RBRACKET; t.line = sl; t.column = sc; ta2_push(&result, t); }
        else if (c == '{') { ADV2(); Token2 t = {0}; t.type = TOK_LBRACE; t.line = sl; t.column = sc; ta2_push(&result, t); }
        else if (c == '}') { ADV2(); Token2 t = {0}; t.type = TOK_RBRACE; t.line = sl; t.column = sc; ta2_push(&result, t); }
        else if (c == '(') { ADV2(); Token2 t = {0}; t.type = TOK_LPAREN; t.line = sl; t.column = sc; ta2_push(&result, t); }
        else if (c == ')') { ADV2(); Token2 t = {0}; t.type = TOK_RPAREN; t.line = sl; t.column = sc; ta2_push(&result, t); }
        else if (c == ',') { ADV2(); Token2 t = {0}; t.type = TOK_COMMA; t.line = sl; t.column = sc; ta2_push(&result, t); }
        else if (c == ':') { ADV2(); Token2 t = {0}; t.type = TOK_COLON; t.line = sl; t.column = sc; ta2_push(&result, t); }
        else if (c == '=') { ADV2(); Token2 t = {0}; t.type = TOK_EQUALS; t.line = sl; t.column = sc; ta2_push(&result, t); }
        else if (c == '&') { ADV2(); Token2 t = {0}; t.type = TOK_AMPERSAND; t.line = sl; t.column = sc; ta2_push(&result, t); }
        else if (c == '|') { ADV2(); Token2 t = {0}; t.type = TOK_PIPE; t.line = sl; t.column = sc; ta2_push(&result, t); }

        /* Variant tag .Identifier */
        else if (c == '.') {
            ADV2();
            char next = CUR2();
            if (isalpha((unsigned char)next) || next == '_') {
                size_t bcap = 64, blen = 0;
                char *buf = malloc(bcap);
                while (pos < len) {
                    char cc = CUR2();
                    if (isalnum((unsigned char)cc) || cc == '_') {
                        buf[blen++] = cc; ADV2();
                    } else break;
                    if (blen >= bcap - 1) { bcap *= 2; buf = realloc(buf, bcap); }
                }
                buf[blen] = '\0';
                Token2 t = {0}; t.type = TOK_VARIANT_TAG; t.text = buf; t.text_len = blen;
                t.line = sl; t.column = sc;
                ta2_push(&result, t);
            } else {
                Token2 t = {0}; t.type = TOK_DOT; t.line = sl; t.column = sc;
                ta2_push(&result, t);
            }
        }

        /* String */
        else if (c == '"' || c == '\'') {
            char quote = c;
            ADV2();
            size_t bcap = 64, blen = 0;
            char *buf = malloc(bcap);
            bool str_error = false;
            int err_line = 0, err_col = 0;
            char *err_msg = NULL;
            bool terminated = false;
            while (pos < len) {
                char cc = CUR2();
                if (cc == quote) { ADV2(); terminated = true; break; }
                if (cc == '\\') {
                    int esc_line = line, esc_col = col;
                    ADV2();
                    if (pos >= len) {
                        /* Unterminated at end */
                        str_error = true;
                        err_line = line; err_col = col;
                        err_msg = strdup("unterminated string (missing closing quote)");
                        break;
                    }
                    char esc = CUR2(); ADV2();
                    if (esc == '\\') buf[blen++] = '\\';
                    else if (esc == quote) buf[blen++] = quote;
                    else {
                        /* Invalid escape — record error but continue for non-error path */
                        if (!str_error) {
                            str_error = true;
                            err_line = esc_line; err_col = esc_col + 1;
                            err_msg = strdup("unexpected escape sequence in string");
                        }
                        buf[blen++] = esc;
                    }
                } else {
                    buf[blen++] = cc; ADV2();
                }
                if (blen >= bcap - 1) { bcap *= 2; buf = realloc(buf, bcap); }
            }
            if (!terminated && !str_error) {
                str_error = true;
                err_line = line; err_col = col;
                err_msg = strdup("unterminated string (missing closing quote)");
            }
            buf[blen] = '\0';
            if (str_error) {
                /* Emit error token followed by a string token */
                Token2 te = {0}; te.type = TOK_ERROR; te.text = err_msg;
                te.line = err_line; te.column = err_col;
                ta2_push(&result, te);
                /* Also push the string so non-error path still works */
                Token2 ts2 = {0}; ts2.type = TOK_STRING; ts2.text = buf; ts2.text_len = blen;
                ts2.line = sl; ts2.column = sc;
                ta2_push(&result, ts2);
            } else {
                Token2 t = {0}; t.type = TOK_STRING; t.text = buf; t.text_len = blen;
                t.line = sl; t.column = sc;
                ta2_push(&result, t);
            }
        }

        /* Blob 0x... */
        else if (c == '0' && PEEK2(1) == 'x') {
            ADV2(); ADV2();
            size_t bcap = 64, blen = 0;
            char *buf = malloc(bcap);
            while (pos < len && isxdigit((unsigned char)CUR2())) {
                buf[blen++] = CUR2(); ADV2();
                if (blen >= bcap - 1) { bcap *= 2; buf = realloc(buf, bcap); }
            }
            buf[blen] = '\0';
            Token2 t = {0}; t.type = TOK_HEX; t.text = buf; t.text_len = blen;
            t.line = sl; t.column = sc;
            ta2_push(&result, t);
        }

        /* Number or datetime */
        else if (isdigit((unsigned char)c) || (c == '-' && (isdigit((unsigned char)PEEK2(1)) || PEEK2(1) == 'I'))) {
            /* Check -Infinity */
            if (c == '-' && pos + 9 <= len && memcmp(text + pos + 1, "Infinity", 8) == 0) {
                for (int i = 0; i < 9; i++) ADV2();
                Token2 t = {0}; t.type = TOK_FLOAT; t.float_val = -INFINITY;
                t.text = strdup("-Infinity"); t.text_len = 9;
                t.line = sl; t.column = sc;
                ta2_push(&result, t);
            } else {
                /* Collect number/datetime chars */
                size_t bcap = 64, blen = 0;
                char *buf = malloc(bcap);
                bool has_t = false;
                while (pos < len) {
                    char cc = CUR2();
                    if (cc == ':') {
                        if (has_t || memchr(buf, '-', blen)) {
                            buf[blen++] = cc; ADV2();
                        } else break;
                    } else if (isdigit((unsigned char)cc) || cc == '+' || cc == '-' ||
                               cc == '.' || cc == 'T' || cc == 'Z' || cc == 'e' || cc == 'E') {
                        if (cc == 'T') has_t = true;
                        buf[blen++] = cc; ADV2();
                    } else break;
                    if (blen >= bcap - 1) { bcap *= 2; buf = realloc(buf, bcap); }
                }
                buf[blen] = '\0';
                Token2 t = {0};
                t.line = sl; t.column = sc;

                /* Check for backreference: integer immediately followed by # */
                if (pos < len && text[pos] == '#' && !has_t &&
                    !memchr(buf, '.', blen) && !memchr(buf, ':', blen)) {
                    buf[blen++] = '#';
                    if (blen >= bcap) { bcap *= 2; buf = realloc(buf, bcap); }
                    ADV2();
                    /* Consume path components: .identifier or [content] */
                    while (pos < len) {
                        char cc = CUR2();
                        if (cc == '.') {
                            if (blen >= bcap - 1) { bcap *= 2; buf = realloc(buf, bcap); }
                            buf[blen++] = cc; ADV2();
                            while (pos < len && (isalnum((unsigned char)CUR2()) || CUR2() == '_')) {
                                if (blen >= bcap - 1) { bcap *= 2; buf = realloc(buf, bcap); }
                                buf[blen++] = CUR2(); ADV2();
                            }
                        } else if (cc == '[') {
                            int depth2 = 1;
                            if (blen >= bcap - 1) { bcap *= 2; buf = realloc(buf, bcap); }
                            buf[blen++] = cc; ADV2();
                            while (pos < len && depth2 > 0) {
                                cc = CUR2();
                                if (cc == '[') depth2++;
                                else if (cc == ']') depth2--;
                                if (blen >= bcap - 1) { bcap *= 2; buf = realloc(buf, bcap); }
                                buf[blen++] = cc; ADV2();
                            }
                        } else {
                            break;
                        }
                    }
                    buf[blen] = '\0';
                    t.type = TOK_BACKREF;
                    t.text = buf; t.text_len = blen;
                    ta2_push(&result, t);
                } else if (has_t || (memchr(buf, ':', blen) && memchr(buf, '-', blen))) {
                    t.type = TOK_DATETIME_LIT;
                    t.text = buf; t.text_len = blen;
                    ta2_push(&result, t);
                } else if (memchr(buf, '.', blen) || memchr(buf, 'e', blen) || memchr(buf, 'E', blen)) {
                    t.type = TOK_FLOAT;
                    t.float_val = strtod(buf, NULL);
                    t.text = buf; t.text_len = blen;
                    ta2_push(&result, t);
                } else {
                    t.type = TOK_INTEGER;
                    t.int_val = strtoll(buf, NULL, 10);
                    t.text = buf; t.text_len = blen;
                    ta2_push(&result, t);
                }
            }
        }

        /* Identifier/keyword */
        else if (isalpha((unsigned char)c) || c == '_' || c == '`') {
            if (c == '`') {
                ADV2();
                size_t bcap = 64, blen = 0;
                char *buf = malloc(bcap);
                while (pos < len && CUR2() != '`') {
                    buf[blen++] = CUR2(); ADV2();
                    if (blen >= bcap - 1) { bcap *= 2; buf = realloc(buf, bcap); }
                }
                if (CUR2() == '`') ADV2();
                buf[blen] = '\0';
                Token2 t = {0}; t.type = TOK_IDENTIFIER; t.text = buf; t.text_len = blen;
                t.line = sl; t.column = sc;
                ta2_push(&result, t);
            } else {
                size_t bcap = 64, blen = 0;
                char *buf = malloc(bcap);
                while (pos < len && (isalnum((unsigned char)CUR2()) || CUR2() == '_')) {
                    buf[blen++] = CUR2(); ADV2();
                    if (blen >= bcap - 1) { bcap *= 2; buf = realloc(buf, bcap); }
                }
                buf[blen] = '\0';
                Token2 t = {0}; t.text = buf; t.text_len = blen;
                t.line = sl; t.column = sc;

                if (strcmp(buf, "null") == 0) t.type = TOK_NULL_TOK;
                else if (strcmp(buf, "true") == 0) t.type = TOK_TRUE;
                else if (strcmp(buf, "false") == 0) t.type = TOK_FALSE;
                else if (strcmp(buf, "NaN") == 0) { t.type = TOK_FLOAT; t.float_val = NAN; }
                else if (strcmp(buf, "Infinity") == 0) { t.type = TOK_FLOAT; t.float_val = INFINITY; }
                else t.type = TOK_IDENTIFIER;

                ta2_push(&result, t);
            }
        } else {
            ADV2(); /* skip unrecognized */
        }
    }

    #undef ADV2
    #undef CUR2
    #undef PEEK2

    return result;
}

/* ================================================================== */
/*  Stream helpers                                                     */
/* ================================================================== */

static TokStream2 ts2_new(const char *text)
{
    TokStream2 ts;
    ts.ta = tokenize2(text);
    ts.pos = 0;
    return ts;
}

static void ts2_free(TokStream2 *ts)
{
    ta2_free(&ts->ta);
}

static Token2 *ts2_cur(TokStream2 *ts)
{
    if (ts->pos >= ts->ta.count) return &ts->ta.tokens[ts->ta.count - 1];
    return &ts->ta.tokens[ts->pos];
}

static Token2 *ts2_adv(TokStream2 *ts)
{
    Token2 *t = ts2_cur(ts);
    if (ts->pos < ts->ta.count - 1) ts->pos++;
    return t;
}

static bool ts2_match(TokStream2 *ts, EastTokenType2 type)
{
    if (ts2_cur(ts)->type == type) { ts2_adv(ts); return true; }
    return false;
}

/* ================================================================== */
/*  Parse context for alias / backreference tracking                   */
/* ================================================================== */

typedef struct {
    EastValue *value;
    char **path;
    size_t path_len;
    size_t path_cap;
} ParseRefEntry;

typedef struct {
    ParseRefEntry *refs;
    size_t ref_count;
    size_t ref_cap;
    char **path;
    size_t path_depth;
    size_t path_cap;
} ParseContext;

static void pctx_push_path(ParseContext *ctx, const char *component) {
    if (!ctx) return;
    if (ctx->path_depth >= ctx->path_cap) {
        size_t new_cap = ctx->path_cap ? ctx->path_cap * 2 : 8;
        ctx->path = realloc(ctx->path, new_cap * sizeof(char *));
        ctx->path_cap = new_cap;
    }
    ctx->path[ctx->path_depth++] = strdup(component);
}

static void pctx_pop_path(ParseContext *ctx) {
    if (ctx && ctx->path_depth > 0) {
        free(ctx->path[--ctx->path_depth]);
    }
}

static void pctx_register(ParseContext *ctx, EastValue *val) {
    if (!ctx) return;
    if (ctx->ref_count >= ctx->ref_cap) {
        size_t new_cap = ctx->ref_cap ? ctx->ref_cap * 2 : 8;
        ctx->refs = realloc(ctx->refs, new_cap * sizeof(ParseRefEntry));
        ctx->ref_cap = new_cap;
    }
    ParseRefEntry *e = &ctx->refs[ctx->ref_count++];
    e->value = val;
    e->path_len = ctx->path_depth;
    e->path_cap = ctx->path_depth ? ctx->path_depth : 0;
    e->path = e->path_cap ? malloc(e->path_cap * sizeof(char *)) : NULL;
    for (size_t i = 0; i < ctx->path_depth; i++) {
        e->path[i] = strdup(ctx->path[i]);
    }
}

static void pctx_free(ParseContext *ctx) {
    if (!ctx) return;
    for (size_t i = 0; i < ctx->ref_count; i++) {
        for (size_t j = 0; j < ctx->refs[i].path_len; j++) {
            free(ctx->refs[i].path[j]);
        }
        free(ctx->refs[i].path);
    }
    free(ctx->refs);
    for (size_t i = 0; i < ctx->path_depth; i++) {
        free(ctx->path[i]);
    }
    free(ctx->path);
}

/* Resolve a backreference token like "1#.a" or "2#[0]" */
static EastValue *pctx_resolve_backref(TokStream2 *ts, ParseContext *ctx) {
    Token2 *tok = ts2_cur(ts);
    if (!tok || tok->type != TOK_BACKREF || !ctx || !tok->text) return NULL;
    ts2_adv(ts);

    const char *ref_str = tok->text;
    const char *hash = strchr(ref_str, '#');
    if (!hash) return NULL;

    int up_levels = 0;
    for (const char *p = ref_str; p < hash; p++) {
        up_levels = up_levels * 10 + (*p - '0');
    }

    if (up_levels > (int)ctx->path_depth) return NULL;
    size_t target_base = ctx->path_depth - (size_t)up_levels;

    /* Parse remaining path components after # */
    const char *remaining = hash + 1;
    size_t rem_cap = 8;
    char **rem_comps = malloc(rem_cap * sizeof(char *));
    size_t num_rem = 0;

    const char *p = remaining;
    while (*p) {
        if (*p == '.') {
            const char *start = p;
            p++;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
            size_t clen = (size_t)(p - start);
            if (num_rem >= rem_cap) {
                rem_cap *= 2;
                rem_comps = realloc(rem_comps, rem_cap * sizeof(char *));
            }
            rem_comps[num_rem] = malloc(clen + 1);
            memcpy(rem_comps[num_rem], start, clen);
            rem_comps[num_rem][clen] = '\0';
            num_rem++;
        } else if (*p == '[') {
            const char *start = p;
            p++;
            int depth = 1;
            while (*p && depth > 0) {
                if (*p == '[') depth++;
                else if (*p == ']') depth--;
                p++;
            }
            size_t clen = (size_t)(p - start);
            if (num_rem >= rem_cap) {
                rem_cap *= 2;
                rem_comps = realloc(rem_comps, rem_cap * sizeof(char *));
            }
            rem_comps[num_rem] = malloc(clen + 1);
            memcpy(rem_comps[num_rem], start, clen);
            rem_comps[num_rem][clen] = '\0';
            num_rem++;
        } else {
            p++;
        }
    }

    size_t target_len = target_base + num_rem;

    /* Find matching ref entry */
    for (size_t i = 0; i < ctx->ref_count; i++) {
        ParseRefEntry *e = &ctx->refs[i];
        if (e->path_len != target_len) continue;

        bool match = true;
        for (size_t j = 0; j < target_base && match; j++) {
            if (strcmp(e->path[j], ctx->path[j]) != 0) match = false;
        }
        for (size_t j = 0; j < num_rem && match; j++) {
            if (strcmp(e->path[target_base + j], rem_comps[j]) != 0) match = false;
        }

        if (match) {
            for (size_t j = 0; j < num_rem; j++) free(rem_comps[j]);
            free(rem_comps);
            east_value_retain(e->value);
            return e->value;
        }
    }

    for (size_t j = 0; j < num_rem; j++) free(rem_comps[j]);
    free(rem_comps);
    return NULL;
}

/* ================================================================== */
/*  Value parser                                                       */
/* ================================================================== */

static EastValue *parse_val(TokStream2 *ts, EastType *type, ParseContext *ctx);

static EastValue *parse_val(TokStream2 *ts, EastType *type, ParseContext *ctx)
{
    if (!type) return NULL;
    Token2 *tok = ts2_cur(ts);

    switch (type->kind) {
    case EAST_TYPE_NULL:
        if (tok->type == TOK_NULL_TOK) { ts2_adv(ts); return east_null(); }
        return NULL;

    case EAST_TYPE_BOOLEAN:
        if (tok->type == TOK_TRUE) { ts2_adv(ts); return east_boolean(true); }
        if (tok->type == TOK_FALSE) { ts2_adv(ts); return east_boolean(false); }
        return NULL;

    case EAST_TYPE_INTEGER:
        if (tok->type == TOK_INTEGER) { ts2_adv(ts); return east_integer(tok->int_val); }
        return NULL;

    case EAST_TYPE_FLOAT:
        if (tok->type == TOK_FLOAT) { ts2_adv(ts); return east_float(tok->float_val); }
        if (tok->type == TOK_INTEGER) { ts2_adv(ts); return east_float((double)tok->int_val); }
        return NULL;

    case EAST_TYPE_STRING:
        if (tok->type == TOK_ERROR) {
            /* Skip error token to get the string token behind it */
            ts2_adv(ts);
            tok = ts2_cur(ts);
        }
        if (tok->type == TOK_STRING) {
            ts2_adv(ts);
            return east_string_len(tok->text, tok->text_len);
        }
        return NULL;

    case EAST_TYPE_DATETIME: {
        if (tok->type == TOK_DATETIME_LIT && tok->text) {
            ts2_adv(ts);
            /* Parse ISO 8601 datetime */
            int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0, ms = 0;
            int tz_sign = 1, tz_hour = 0, tz_min = 0;

            sscanf(tok->text, "%d-%d-%dT%d:%d:%d",
                   &year, &month, &day, &hour, &min, &sec);
            const char *dot = strchr(tok->text, '.');
            if (dot) {
                /* Parse milliseconds */
                char msbuf[4] = {0};
                size_t mlen = 0;
                const char *dp = dot + 1;
                while (*dp >= '0' && *dp <= '9' && mlen < 3) {
                    msbuf[mlen++] = *dp++;
                }
                /* Pad with zeros if needed */
                while (mlen < 3) msbuf[mlen++] = '0';
                ms = atoi(msbuf);
            }

            /* Handle timezone */
            const char *p = tok->text;
            while (*p) {
                if (*p == 'Z' || *p == 'z') break;
                if ((*p == '+' || *p == '-') && p > tok->text + 10) {
                    tz_sign = (*p == '-') ? -1 : 1;
                    sscanf(p + 1, "%d:%d", &tz_hour, &tz_min);
                    break;
                }
                p++;
            }

            /* Convert to epoch millis */
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

            return east_datetime(epoch_secs * 1000 + ms);
        }
        return NULL;
    }

    case EAST_TYPE_BLOB: {
        if (tok->type == TOK_HEX && tok->text) {
            ts2_adv(ts);
            size_t hlen = tok->text_len;
            size_t blen = hlen / 2;
            if (hlen % 2 != 0) return NULL;
            if (hlen == 0) return east_blob(NULL, 0);

            uint8_t *bdata = malloc(blen);
            for (size_t i = 0; i < blen; i++) {
                char hex[3] = { tok->text[i*2], tok->text[i*2+1], '\0' };
                bdata[i] = (uint8_t)strtoul(hex, NULL, 16);
            }
            EastValue *val = east_blob(bdata, blen);
            free(bdata);
            return val;
        }
        return NULL;
    }

    case EAST_TYPE_ARRAY: {
        if (ctx && ts2_cur(ts)->type == TOK_BACKREF)
            return pctx_resolve_backref(ts, ctx);
        EastType *elem_type = type->data.element;
        if (!ts2_match(ts, TOK_LBRACKET)) return NULL;
        EastValue *arr = east_array_new(elem_type);
        if (ctx) pctx_register(ctx, arr);

        if (ts2_cur(ts)->type != TOK_RBRACKET) {
            size_t idx = 0;
            for (;;) {
                char idx_buf[24];
                snprintf(idx_buf, sizeof(idx_buf), "[%zu]", idx);
                if (ctx) pctx_push_path(ctx, idx_buf);
                EastValue *elem = parse_val(ts, elem_type, ctx);
                if (ctx) pctx_pop_path(ctx);
                if (!elem) { east_value_release(arr); return NULL; }
                east_array_push(arr, elem);
                east_value_release(elem);
                idx++;
                if (!ts2_match(ts, TOK_COMMA)) break;
            }
        }
        if (!ts2_match(ts, TOK_RBRACKET)) { east_value_release(arr); return NULL; }
        return arr;
    }

    case EAST_TYPE_SET: {
        if (ctx && ts2_cur(ts)->type == TOK_BACKREF)
            return pctx_resolve_backref(ts, ctx);
        EastType *elem_type = type->data.element;
        if (!ts2_match(ts, TOK_LBRACE)) return NULL;
        EastValue *set = east_set_new(elem_type);
        if (ctx) pctx_register(ctx, set);

        if (ts2_cur(ts)->type != TOK_RBRACE) {
            for (;;) {
                EastValue *elem = parse_val(ts, elem_type, ctx);
                if (!elem) { east_value_release(set); return NULL; }
                east_set_insert(set, elem);
                east_value_release(elem);
                if (!ts2_match(ts, TOK_COMMA)) break;
            }
        }
        if (!ts2_match(ts, TOK_RBRACE)) { east_value_release(set); return NULL; }
        return set;
    }

    case EAST_TYPE_DICT: {
        if (ctx && ts2_cur(ts)->type == TOK_BACKREF)
            return pctx_resolve_backref(ts, ctx);
        EastType *key_type = type->data.dict.key;
        EastType *val_type = type->data.dict.value;

        if (!ts2_match(ts, TOK_LBRACE)) return NULL;
        EastValue *dict = east_dict_new(key_type, val_type);
        if (ctx) pctx_register(ctx, dict);

        /* Handle empty dict: {} or {:} */
        if (ts2_cur(ts)->type == TOK_RBRACE) {
            ts2_adv(ts);
            return dict;
        }
        if (ts2_cur(ts)->type == TOK_COLON) {
            ts2_adv(ts);
            if (!ts2_match(ts, TOK_RBRACE)) { east_value_release(dict); return NULL; }
            return dict;
        }

        for (;;) {
            EastValue *k = parse_val(ts, key_type, ctx);
            if (!k) { east_value_release(dict); return NULL; }
            if (!ts2_match(ts, TOK_COLON)) {
                east_value_release(k); east_value_release(dict); return NULL;
            }
            EastValue *v = parse_val(ts, val_type, ctx);
            if (!v) {
                east_value_release(k); east_value_release(dict); return NULL;
            }
            east_dict_set(dict, k, v);
            east_value_release(k);
            east_value_release(v);
            if (!ts2_match(ts, TOK_COMMA)) break;
        }
        if (!ts2_match(ts, TOK_RBRACE)) { east_value_release(dict); return NULL; }
        return dict;
    }

    case EAST_TYPE_STRUCT: {
        if (!ts2_match(ts, TOK_LPAREN)) return NULL;

        size_t nf = type->data.struct_.num_fields;
        const char **names = malloc(nf * sizeof(char *));
        EastValue **values = calloc(nf, sizeof(EastValue *));

        for (size_t i = 0; i < nf; i++) {
            names[i] = type->data.struct_.fields[i].name;
        }

        while (ts2_cur(ts)->type != TOK_RPAREN &&
               ts2_cur(ts)->type != TOK_EOF_TOK) {
            /* Parse field_name = value */
            Token2 *name_tok = ts2_cur(ts);
            if (name_tok->type != TOK_IDENTIFIER) break;
            ts2_adv(ts);

            if (!ts2_match(ts, TOK_EQUALS)) break;

            /* Find field index */
            int fidx = -1;
            for (size_t i = 0; i < nf; i++) {
                if (strcmp(names[i], name_tok->text) == 0) {
                    fidx = (int)i;
                    break;
                }
            }

            if (fidx >= 0) {
                char path_buf[256];
                snprintf(path_buf, sizeof(path_buf), ".%s", name_tok->text);
                if (ctx) pctx_push_path(ctx, path_buf);
                values[fidx] = parse_val(ts, type->data.struct_.fields[fidx].type, ctx);
                if (ctx) pctx_pop_path(ctx);
                if (!values[fidx]) {
                    /* Field was present but value failed to parse — error */
                    for (size_t i = 0; i < nf; i++) {
                        if (values[i]) east_value_release(values[i]);
                    }
                    free(names);
                    free(values);
                    return NULL;
                }
            }

            ts2_match(ts, TOK_COMMA); /* optional trailing comma */
        }

        ts2_match(ts, TOK_RPAREN);

        /* Fill missing fields with null */
        for (size_t i = 0; i < nf; i++) {
            if (!values[i]) values[i] = east_null();
        }

        EastValue *result = east_struct_new(names, values, nf, type);
        for (size_t i = 0; i < nf; i++) east_value_release(values[i]);
        free(names);
        free(values);
        return result;
    }

    case EAST_TYPE_VARIANT: {
        /* .CaseName [value] */
        if (tok->type != TOK_VARIANT_TAG) return NULL;
        ts2_adv(ts);

        const char *case_name = tok->text;
        EastType *case_type = NULL;
        for (size_t i = 0; i < type->data.variant.num_cases; i++) {
            if (strcmp(type->data.variant.cases[i].name, case_name) == 0) {
                case_type = type->data.variant.cases[i].type;
                break;
            }
        }
        if (!case_type) return NULL;

        EastValue *case_value;
        if (case_type->kind == EAST_TYPE_NULL) {
            /* Nullary variant: optionally accept explicit "null" */
            if (ts2_cur(ts)->type == TOK_NULL_TOK) ts2_adv(ts);
            case_value = east_null();
        } else {
            case_value = parse_val(ts, case_type, ctx);
            if (!case_value) return NULL;
        }

        EastValue *result = east_variant_new(case_name, case_value, type);
        east_value_release(case_value);
        return result;
    }

    case EAST_TYPE_REF: {
        /* &value or backref */
        if (ctx && ts2_cur(ts)->type == TOK_BACKREF)
            return pctx_resolve_backref(ts, ctx);
        if (!ts2_match(ts, TOK_AMPERSAND)) return NULL;
        EastValue *inner = parse_val(ts, type->data.element, ctx);
        if (!inner) return NULL;
        EastValue *ref = east_ref_new(inner);
        east_value_release(inner);
        if (ctx) pctx_register(ctx, ref);
        return ref;
    }

    case EAST_TYPE_VECTOR: {
        /* vec[elem, elem, ...] */
        EastType *elem_type = type->data.element;
        Token2 *cur = ts2_cur(ts);
        if (cur->type != TOK_IDENTIFIER || !cur->text || strcmp(cur->text, "vec") != 0)
            return NULL;
        ts2_adv(ts);
        if (!ts2_match(ts, TOK_LBRACKET)) return NULL;

        /* Collect elements */
        size_t cap = 16, vlen = 0;
        size_t elem_size = 0;
        if (elem_type->kind == EAST_TYPE_FLOAT) elem_size = sizeof(double);
        else if (elem_type->kind == EAST_TYPE_INTEGER) elem_size = sizeof(int64_t);
        else if (elem_type->kind == EAST_TYPE_BOOLEAN) elem_size = sizeof(bool);

        void *tmp = malloc(cap * elem_size);

        if (ts2_cur(ts)->type != TOK_RBRACKET) {
            for (;;) {
                EastValue *elem = parse_val(ts, elem_type, ctx);
                if (!elem) { free(tmp); return NULL; }
                if (vlen >= cap) {
                    cap *= 2;
                    tmp = realloc(tmp, cap * elem_size);
                }
                if (elem_type->kind == EAST_TYPE_FLOAT)
                    ((double *)tmp)[vlen] = elem->data.float64;
                else if (elem_type->kind == EAST_TYPE_INTEGER)
                    ((int64_t *)tmp)[vlen] = elem->data.integer;
                else if (elem_type->kind == EAST_TYPE_BOOLEAN)
                    ((bool *)tmp)[vlen] = elem->data.boolean;
                east_value_release(elem);
                vlen++;
                if (!ts2_match(ts, TOK_COMMA)) break;
            }
        }
        if (!ts2_match(ts, TOK_RBRACKET)) { free(tmp); return NULL; }

        EastValue *vec = east_vector_new(elem_type, vlen);
        if (vec && vlen > 0) {
            memcpy(vec->data.vector.data, tmp, vlen * elem_size);
        }
        free(tmp);
        return vec;
    }

    case EAST_TYPE_MATRIX: {
        /* mat[[row], [row], ...] */
        EastType *elem_type = type->data.element;
        Token2 *cur = ts2_cur(ts);
        if (cur->type != TOK_IDENTIFIER || !cur->text || strcmp(cur->text, "mat") != 0)
            return NULL;
        ts2_adv(ts);
        if (!ts2_match(ts, TOK_LBRACKET)) return NULL;

        size_t rows = 0, cols = 0;
        size_t cap_flat = 64;
        size_t elem_size = 0;
        if (elem_type->kind == EAST_TYPE_FLOAT) elem_size = sizeof(double);
        else if (elem_type->kind == EAST_TYPE_INTEGER) elem_size = sizeof(int64_t);
        else if (elem_type->kind == EAST_TYPE_BOOLEAN) elem_size = sizeof(bool);

        void *flat = malloc(cap_flat * elem_size);
        size_t flat_len = 0;

        if (ts2_cur(ts)->type != TOK_RBRACKET) {
            for (;;) {
                if (!ts2_match(ts, TOK_LBRACKET)) { free(flat); return NULL; }
                size_t row_cols = 0;
                if (ts2_cur(ts)->type != TOK_RBRACKET) {
                    for (;;) {
                        if (flat_len >= cap_flat) {
                            cap_flat *= 2;
                            flat = realloc(flat, cap_flat * elem_size);
                        }
                        EastValue *elem = parse_val(ts, elem_type, ctx);
                        if (!elem) { free(flat); return NULL; }
                        if (elem_type->kind == EAST_TYPE_FLOAT)
                            ((double *)flat)[flat_len] = elem->data.float64;
                        else if (elem_type->kind == EAST_TYPE_INTEGER)
                            ((int64_t *)flat)[flat_len] = elem->data.integer;
                        else if (elem_type->kind == EAST_TYPE_BOOLEAN)
                            ((bool *)flat)[flat_len] = elem->data.boolean;
                        east_value_release(elem);
                        flat_len++;
                        row_cols++;
                        if (!ts2_match(ts, TOK_COMMA)) break;
                    }
                }
                if (!ts2_match(ts, TOK_RBRACKET)) { free(flat); return NULL; }
                if (rows == 0) cols = row_cols;
                rows++;
                if (!ts2_match(ts, TOK_COMMA)) break;
            }
        }
        if (!ts2_match(ts, TOK_RBRACKET)) { free(flat); return NULL; }

        EastValue *mat = east_matrix_new(elem_type, rows, cols);
        if (mat && flat_len > 0) {
            memcpy(mat->data.matrix.data, flat, flat_len * elem_size);
        }
        free(flat);
        return mat;
    }

    case EAST_TYPE_RECURSIVE:
        /* Unwrap: parse via the inner node type */
        if (type->data.recursive.node) {
            return parse_val(ts, type->data.recursive.node, ctx);
        }
        return NULL;

    case EAST_TYPE_NEVER:
    case EAST_TYPE_FUNCTION:
    case EAST_TYPE_ASYNC_FUNCTION:
        return NULL;
    }

    return NULL;
}

/* ================================================================== */
/*  Public API: east_parse_value                                       */
/* ================================================================== */

EastValue *east_parse_value(const char *text, EastType *type)
{
    if (!text || !type) return NULL;
    TokStream2 ts = ts2_new(text);
    ParseContext ctx = {0};
    EastValue *result = parse_val(&ts, type, &ctx);
    pctx_free(&ctx);
    ts2_free(&ts);
    return result;
}

/* ================================================================== */
/*  Error-enhanced parser                                              */
/* ================================================================== */

typedef struct {
    char *message;   /* e.g. "expected null, got '1'" */
    char *path;      /* e.g. "[1].fieldname" or NULL */
    int line;
    int column;
} ParseErr;

static void pe_init(ParseErr *e) { e->message = NULL; e->path = NULL; e->line = 1; e->column = 1; }

static void pe_free(ParseErr *e) {
    free(e->message);
    free(e->path);
    e->message = NULL;
    e->path = NULL;
}

static void pe_set(ParseErr *e, char *msg, int line, int col) {
    free(e->message);
    e->message = msg;
    free(e->path);
    e->path = NULL;
    e->line = line;
    e->column = col;
}

static void pe_prepend_path(ParseErr *e, const char *segment) {
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

/* Format "got" for a token: either 'c' for the first char, or "end of input" */
static char *pe_got_token(Token2 *tok, const char *input) {
    if (tok->type == TOK_EOF_TOK) return strdup("end of input");
    /* Get the character at the token's position in the original input */
    /* We can reconstruct from token line/col, but simpler: use the token text or type */
    /* For single-char tokens, we know what they are */
    char c = '\0';
    switch (tok->type) {
        case TOK_LBRACKET: c = '['; break;
        case TOK_RBRACKET: c = ']'; break;
        case TOK_LBRACE: c = '{'; break;
        case TOK_RBRACE: c = '}'; break;
        case TOK_LPAREN: c = '('; break;
        case TOK_RPAREN: c = ')'; break;
        case TOK_COMMA: c = ','; break;
        case TOK_COLON: c = ':'; break;
        case TOK_EQUALS: c = '='; break;
        case TOK_AMPERSAND: c = '&'; break;
        case TOK_PIPE: c = '|'; break;
        case TOK_DOT: c = '.'; break;
        default: break;
    }
    if (c) {
        char buf[8];
        snprintf(buf, sizeof(buf), "'%c'", c);
        return strdup(buf);
    }
    /* For multi-char tokens, get the first character from input text at the token's position */
    if (input) {
        /* Find position in input by scanning to line/col */
        int line = 1, col = 1;
        const char *p = input;
        while (*p) {
            if (line == tok->line && col == tok->column) {
                char buf[8];
                snprintf(buf, sizeof(buf), "'%c'", *p);
                return strdup(buf);
            }
            if (*p == '\n') { line++; col = 1; } else { col++; }
            p++;
        }
    }
    /* Fallback: use first char of token text */
    if (tok->text && tok->text[0]) {
        char buf[8];
        snprintf(buf, sizeof(buf), "'%c'", tok->text[0]);
        return strdup(buf);
    }
    return strdup("end of input");
}

static EastValue *parse_val_err(TokStream2 *ts, EastType *type, ParseContext *ctx,
                                 ParseErr *err, const char *input);

static EastValue *parse_val_err(TokStream2 *ts, EastType *type, ParseContext *ctx,
                                 ParseErr *err, const char *input)
{
    if (!type) return NULL;
    Token2 *tok = ts2_cur(ts);

    switch (type->kind) {
    case EAST_TYPE_NULL:
        if (tok->type == TOK_NULL_TOK) { ts2_adv(ts); return east_null(); }
        if (err) {
            char *got = pe_got_token(tok, input);
            size_t len = 30 + strlen(got);
            char *msg = malloc(len);
            snprintf(msg, len, "expected null, got %s", got);
            free(got);
            pe_set(err, msg, tok->line, tok->column);
        }
        return NULL;

    case EAST_TYPE_BOOLEAN:
        if (tok->type == TOK_TRUE) { ts2_adv(ts); return east_boolean(true); }
        if (tok->type == TOK_FALSE) { ts2_adv(ts); return east_boolean(false); }
        if (err) {
            char *got = pe_got_token(tok, input);
            size_t len = 30 + strlen(got);
            char *msg = malloc(len);
            snprintf(msg, len, "expected boolean, got %s", got);
            free(got);
            pe_set(err, msg, tok->line, tok->column);
        }
        return NULL;

    case EAST_TYPE_INTEGER:
        if (tok->type == TOK_INTEGER) {
            /* Check overflow via round-trip */
            if (tok->text) {
                char check[32];
                snprintf(check, sizeof(check), "%lld", (long long)tok->int_val);
                if (strcmp(check, tok->text) != 0) {
                    if (err) {
                        size_t len = 80 + strlen(tok->text);
                        char *msg = malloc(len);
                        snprintf(msg, len, "integer out of range (must be 64-bit signed), got %s", tok->text);
                        pe_set(err, msg, tok->line, tok->column);
                    }
                    return NULL;
                }
            }
            ts2_adv(ts);
            return east_integer(tok->int_val);
        }
        if (err) {
            char *got = pe_got_token(tok, input);
            size_t len = 30 + strlen(got);
            char *msg = malloc(len);
            snprintf(msg, len, "expected integer, got %s", got);
            free(got);
            pe_set(err, msg, tok->line, tok->column);
        }
        return NULL;

    case EAST_TYPE_FLOAT:
        if (tok->type == TOK_FLOAT) {
            /* Check for missing exponent digits: tokenizer stores text */
            if (tok->text) {
                size_t tlen = strlen(tok->text);
                if (tlen > 0 && (tok->text[tlen-1] == 'e' || tok->text[tlen-1] == 'E')) {
                    if (err) {
                        /* Position after the 'e' */
                        pe_set(err, strdup("expected digits in float exponent"),
                               tok->line, tok->column + (int)tlen);
                    }
                    return NULL;
                }
            }
            ts2_adv(ts);
            return east_float(tok->float_val);
        }
        if (tok->type == TOK_INTEGER) { ts2_adv(ts); return east_float((double)tok->int_val); }
        if (err) {
            char *got = pe_got_token(tok, input);
            size_t len = 30 + strlen(got);
            char *msg = malloc(len);
            snprintf(msg, len, "expected float, got %s", got);
            free(got);
            pe_set(err, msg, tok->line, tok->column);
        }
        return NULL;

    case EAST_TYPE_STRING:
        if (tok->type == TOK_ERROR) {
            /* String tokenizer error (bad escape, unterminated) */
            if (err) pe_set(err, strdup(tok->text), tok->line, tok->column);
            /* Skip error token and the string token that follows */
            ts2_adv(ts);
            if (ts2_cur(ts)->type == TOK_STRING) ts2_adv(ts);
            return NULL;
        }
        if (tok->type == TOK_STRING) {
            ts2_adv(ts);
            return east_string_len(tok->text, tok->text_len);
        }
        if (err) {
            char *got = pe_got_token(tok, input);
            size_t len = 30 + strlen(got);
            char *msg = malloc(len);
            snprintf(msg, len, "expected '\"', got %s", got);
            free(got);
            pe_set(err, msg, tok->line, tok->column);
        }
        return NULL;

    case EAST_TYPE_DATETIME: {
        if (tok->type == TOK_DATETIME_LIT && tok->text) {
            /* Validate format and date values */
            int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0, ms = 0;
            int tz_sign = 1, tz_hour = 0, tz_min = 0;
            if (sscanf(tok->text, "%d-%d-%dT%d:%d:%d",
                       &year, &month, &day, &hour, &min, &sec) < 6) {
                if (err) pe_set(err, strdup("expected DateTime in format YYYY-MM-DDTHH:MM:SS.sss"),
                               tok->line, tok->column);
                return NULL;
            }
            /* Check for invalid values */
            if (month < 1 || month > 12 || day < 1 || day > 31 ||
                hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 59) {
                if (err) {
                    size_t len = 60 + strlen(tok->text);
                    char *msg = malloc(len);
                    snprintf(msg, len, "invalid DateTime value, got \"%s\"", tok->text);
                    pe_set(err, msg, tok->line, tok->column);
                }
                return NULL;
            }
            const char *dot = strchr(tok->text, '.');
            if (dot) {
                char msbuf[4] = {0};
                size_t mlen = 0;
                const char *dp = dot + 1;
                while (*dp >= '0' && *dp <= '9' && mlen < 3) msbuf[mlen++] = *dp++;
                while (mlen < 3) msbuf[mlen++] = '0';
                ms = atoi(msbuf);
            }
            const char *p = tok->text;
            while (*p) {
                if (*p == 'Z' || *p == 'z') break;
                if ((*p == '+' || *p == '-') && p > tok->text + 10) {
                    tz_sign = (*p == '-') ? -1 : 1;
                    sscanf(p + 1, "%d:%d", &tz_hour, &tz_min);
                    break;
                }
                p++;
            }
            int64_t y = year, m_adj = month;
            if (m_adj <= 2) { y--; m_adj += 9; } else { m_adj -= 3; }
            int64_t era = (y >= 0 ? y : y - 399) / 400;
            int64_t yoe = y - era * 400;
            int64_t doy = (153 * m_adj + 2) / 5 + day - 1;
            int64_t doe = yoe * 365 + yoe/4 - yoe/100 + doy;
            int64_t days = era * 146097 + doe - 719468;
            int64_t epoch_secs = days * 86400 + hour * 3600 + min * 60 + sec;
            epoch_secs -= tz_sign * (tz_hour * 3600 + tz_min * 60);
            ts2_adv(ts);
            return east_datetime(epoch_secs * 1000 + ms);
        }
        if (err) pe_set(err, strdup("expected DateTime in format YYYY-MM-DDTHH:MM:SS.sss"),
                       tok->line, tok->column);
        return NULL;
    }

    case EAST_TYPE_BLOB: {
        if (tok->type == TOK_HEX && tok->text) {
            size_t hlen = tok->text_len;
            if (hlen % 2 != 0) {
                if (err) {
                    size_t len = 60 + hlen;
                    char *msg = malloc(len);
                    snprintf(msg, len, "invalid hex string (odd length), got \"0x%s\"", tok->text);
                    pe_set(err, msg, tok->line, tok->column);
                }
                return NULL;
            }
            size_t blen = hlen / 2;
            if (hlen == 0) { ts2_adv(ts); return east_blob(NULL, 0); }
            uint8_t *bdata = malloc(blen);
            for (size_t i = 0; i < blen; i++) {
                char hex[3] = { tok->text[i*2], tok->text[i*2+1], '\0' };
                bdata[i] = (uint8_t)strtoul(hex, NULL, 16);
            }
            EastValue *val = east_blob(bdata, blen);
            free(bdata);
            ts2_adv(ts);
            return val;
        }
        if (err) pe_set(err, strdup("expected Blob starting with 0x"),
                       tok->line, tok->column);
        return NULL;
    }

    case EAST_TYPE_ARRAY: {
        if (ctx && ts2_cur(ts)->type == TOK_BACKREF)
            return pctx_resolve_backref(ts, ctx);
        EastType *elem_type = type->data.element;
        if (!ts2_match(ts, TOK_LBRACKET)) {
            if (err) pe_set(err, strdup("expected '[' to start array"),
                           tok->line, tok->column);
            return NULL;
        }
        EastValue *arr = east_array_new(elem_type);
        if (ctx) pctx_register(ctx, arr);

        if (ts2_cur(ts)->type != TOK_RBRACKET) {
            size_t idx = 0;
            for (;;) {
                char idx_buf[24];
                snprintf(idx_buf, sizeof(idx_buf), "[%zu]", idx);
                if (ctx) pctx_push_path(ctx, idx_buf);

                ParseErr inner = {0};
                EastValue *elem = parse_val_err(ts, elem_type, ctx, err ? &inner : NULL, input);
                if (ctx) pctx_pop_path(ctx);
                if (!elem) {
                    if (err && inner.message) {
                        pe_prepend_path(&inner, idx_buf);
                        *err = inner;
                    }
                    east_value_release(arr);
                    return NULL;
                }
                east_array_push(arr, elem);
                east_value_release(elem);
                idx++;

                Token2 *next = ts2_cur(ts);
                if (next->type == TOK_RBRACKET) break;
                if (next->type == TOK_COMMA) { ts2_adv(ts); continue; }
                if (err) pe_set(err, strdup("expected ',' or ']' after array element"),
                               next->line, next->column);
                east_value_release(arr);
                return NULL;
            }
        }
        if (!ts2_match(ts, TOK_RBRACKET)) {
            east_value_release(arr);
            return NULL;
        }
        return arr;
    }

    case EAST_TYPE_SET: {
        if (ctx && ts2_cur(ts)->type == TOK_BACKREF)
            return pctx_resolve_backref(ts, ctx);
        EastType *elem_type = type->data.element;
        if (!ts2_match(ts, TOK_LBRACE)) {
            if (err) pe_set(err, strdup("expected '{' to start set"),
                           tok->line, tok->column);
            return NULL;
        }
        EastValue *set = east_set_new(elem_type);
        if (ctx) pctx_register(ctx, set);

        if (ts2_cur(ts)->type != TOK_RBRACE) {
            size_t idx = 0;
            for (;;) {
                char idx_buf[24];
                snprintf(idx_buf, sizeof(idx_buf), "[%zu]", idx);

                ParseErr inner = {0};
                EastValue *elem = parse_val_err(ts, elem_type, ctx, err ? &inner : NULL, input);
                if (!elem) {
                    if (err && inner.message) {
                        pe_prepend_path(&inner, idx_buf);
                        *err = inner;
                    }
                    east_value_release(set);
                    return NULL;
                }
                east_set_insert(set, elem);
                east_value_release(elem);
                idx++;

                Token2 *next = ts2_cur(ts);
                if (next->type == TOK_RBRACE) break;
                if (next->type == TOK_COMMA) { ts2_adv(ts); continue; }
                if (err) pe_set(err, strdup("expected ',' or '}' after set element"),
                               next->line, next->column);
                east_value_release(set);
                return NULL;
            }
        }
        if (!ts2_match(ts, TOK_RBRACE)) {
            east_value_release(set);
            return NULL;
        }
        return set;
    }

    case EAST_TYPE_DICT: {
        if (ctx && ts2_cur(ts)->type == TOK_BACKREF)
            return pctx_resolve_backref(ts, ctx);
        EastType *key_type = type->data.dict.key;
        EastType *val_type = type->data.dict.value;

        if (!ts2_match(ts, TOK_LBRACE)) {
            if (err) pe_set(err, strdup("expected '{' to start dict"),
                           tok->line, tok->column);
            return NULL;
        }
        EastValue *dict = east_dict_new(key_type, val_type);
        if (ctx) pctx_register(ctx, dict);

        /* Handle empty dict: {} or {:} */
        if (ts2_cur(ts)->type == TOK_RBRACE) {
            ts2_adv(ts);
            return dict;
        }
        if (ts2_cur(ts)->type == TOK_COLON) {
            Token2 *colon_tok = ts2_cur(ts);
            ts2_adv(ts);
            if (ts2_cur(ts)->type == TOK_RBRACE) {
                ts2_adv(ts);
                return dict;
            }
            if (err) pe_set(err, strdup("expected '}' after ':' in empty dict"),
                           ts2_cur(ts)->line, ts2_cur(ts)->column);
            east_value_release(dict);
            return NULL;
        }

        size_t entry_idx = 0;
        for (;;) {
            char key_path[32];
            snprintf(key_path, sizeof(key_path), "[%zu](key)", entry_idx);

            ParseErr inner = {0};
            EastValue *k = parse_val_err(ts, key_type, ctx, err ? &inner : NULL, input);
            if (!k) {
                if (err && inner.message) {
                    pe_prepend_path(&inner, key_path);
                    *err = inner;
                }
                east_value_release(dict);
                return NULL;
            }

            Token2 *colon_check = ts2_cur(ts);
            if (colon_check->type != TOK_COLON) {
                if (err) {
                    size_t len = 64;
                    char *msg = malloc(len);
                    snprintf(msg, len, "expected ':' after dict key at entry %zu", entry_idx);
                    pe_set(err, msg, colon_check->line, colon_check->column);
                }
                east_value_release(k);
                east_value_release(dict);
                return NULL;
            }
            ts2_adv(ts);

            /* Build path for value: [keyStr] */
            char *key_str = east_print_value(k, key_type);
            size_t vpath_len = strlen(key_str) + 4;
            char *val_path = malloc(vpath_len);
            snprintf(val_path, vpath_len, "[%s]", key_str);

            ParseErr inner2 = {0};
            EastValue *v = parse_val_err(ts, val_type, ctx, err ? &inner2 : NULL, input);
            if (!v) {
                if (err && inner2.message) {
                    pe_prepend_path(&inner2, val_path);
                    *err = inner2;
                }
                free(val_path);
                free(key_str);
                east_value_release(k);
                east_value_release(dict);
                return NULL;
            }
            free(val_path);
            free(key_str);

            east_dict_set(dict, k, v);
            east_value_release(k);
            east_value_release(v);

            Token2 *next = ts2_cur(ts);
            if (next->type == TOK_RBRACE) { ts2_adv(ts); return dict; }
            if (next->type == TOK_COMMA) { ts2_adv(ts); entry_idx++; continue; }
            if (err) pe_set(err, strdup("expected ',' or '}' after dict entry"),
                           next->line, next->column);
            east_value_release(dict);
            return NULL;
        }
    }

    case EAST_TYPE_STRUCT: {
        Token2 *open = ts2_cur(ts);
        if (!ts2_match(ts, TOK_LPAREN)) {
            if (err) pe_set(err, strdup("expected '(' to start struct"),
                           open->line, open->column);
            return NULL;
        }

        size_t nf = type->data.struct_.num_fields;
        const char **names = malloc(nf * sizeof(char *));
        EastValue **values = calloc(nf, sizeof(EastValue *));
        for (size_t i = 0; i < nf; i++)
            names[i] = type->data.struct_.fields[i].name;

        /* Parse fields in declaration order (matching TS behavior) */
        for (size_t fi = 0; fi < nf; fi++) {
            const char *expected_name = names[fi];
            Token2 *cur = ts2_cur(ts);

            /* Check for early closing paren (missing fields) */
            if (cur->type == TOK_RPAREN) {
                if (err) {
                    size_t len = 40 + strlen(expected_name);
                    char *msg = malloc(len);
                    snprintf(msg, len, "missing required field '%s'", expected_name);
                    pe_set(err, msg, cur->line, cur->column);
                }
                for (size_t i = 0; i < nf; i++) if (values[i]) east_value_release(values[i]);
                free(names); free(values);
                return NULL;
            }

            /* Check for EOF */
            if (cur->type == TOK_EOF_TOK) {
                if (err) {
                    size_t len = 40 + strlen(expected_name);
                    char *msg = malloc(len);
                    snprintf(msg, len, "missing required field '%s'", expected_name);
                    pe_set(err, msg, cur->line, cur->column);
                }
                for (size_t i = 0; i < nf; i++) if (values[i]) east_value_release(values[i]);
                free(names); free(values);
                return NULL;
            }

            /* Expect an identifier */
            if (cur->type != TOK_IDENTIFIER) {
                if (err) {
                    size_t len = 40 + strlen(expected_name);
                    char *msg = malloc(len);
                    snprintf(msg, len, "missing required field '%s'", expected_name);
                    pe_set(err, msg, cur->line, cur->column);
                }
                for (size_t i = 0; i < nf; i++) if (values[i]) east_value_release(values[i]);
                free(names); free(values);
                return NULL;
            }

            Token2 *name_tok = ts2_adv(ts);

            /* Check field name matches expected */
            if (strcmp(name_tok->text, expected_name) != 0) {
                if (err) {
                    size_t elen = 60 + strlen(name_tok->text);
                    for (size_t i = 0; i < nf; i++) elen += strlen(names[i]) + 2;
                    char *msg = malloc(elen);
                    int off = snprintf(msg, elen, "unknown field '%s', expected one of: ", name_tok->text);
                    for (size_t i = 0; i < nf; i++) {
                        if (i > 0) off += snprintf(msg + off, elen - off, ", ");
                        off += snprintf(msg + off, elen - off, "%s", names[i]);
                    }
                    pe_set(err, msg, name_tok->line, name_tok->column);
                }
                for (size_t i = 0; i < nf; i++) if (values[i]) east_value_release(values[i]);
                free(names); free(values);
                return NULL;
            }

            /* Check for '=' */
            Token2 *eq_tok = ts2_cur(ts);
            if (eq_tok->type != TOK_EQUALS) {
                if (err) {
                    size_t len = 50 + strlen(name_tok->text);
                    char *msg = malloc(len);
                    snprintf(msg, len, "expected '=' after field name '%s'", name_tok->text);
                    pe_set(err, msg, eq_tok->line, eq_tok->column);
                }
                for (size_t i = 0; i < nf; i++) if (values[i]) east_value_release(values[i]);
                free(names); free(values);
                return NULL;
            }
            ts2_adv(ts);

            /* Parse field value */
            char path_buf[256];
            snprintf(path_buf, sizeof(path_buf), ".%s", name_tok->text);
            if (ctx) pctx_push_path(ctx, path_buf);

            ParseErr inner = {0};
            values[fi] = parse_val_err(ts, type->data.struct_.fields[fi].type, ctx,
                                       err ? &inner : NULL, input);
            if (ctx) pctx_pop_path(ctx);
            if (!values[fi]) {
                if (err && inner.message) {
                    pe_prepend_path(&inner, path_buf);
                    *err = inner;
                }
                for (size_t i = 0; i < nf; i++) if (values[i]) east_value_release(values[i]);
                free(names); free(values);
                return NULL;
            }

            /* Look for comma or closing paren */
            Token2 *sep = ts2_cur(ts);
            if (sep->type == TOK_COMMA) {
                ts2_adv(ts);
                /* If this was the last field, after comma check for missing next field */
                if (fi == nf - 1) {
                    /* All fields parsed but there's more after comma — will fall through
                     * to post-loop ')' check */
                }
            } else if (sep->type == TOK_RPAREN) {
                /* Check if we've parsed all fields */
                if (fi < nf - 1) {
                    if (err) {
                        size_t len = 40 + strlen(names[fi + 1]);
                        char *msg = malloc(len);
                        snprintf(msg, len, "missing required field '%s'", names[fi + 1]);
                        pe_set(err, msg, sep->line, sep->column);
                    }
                    for (size_t i = 0; i < nf; i++) if (values[i]) east_value_release(values[i]);
                    free(names); free(values);
                    return NULL;
                }
                /* All fields parsed and at ')' — break */
                break;
            } else if (sep->type == TOK_EOF_TOK) {
                if (err) pe_set(err, strdup("unexpected end of input in struct"),
                               sep->line, sep->column);
                for (size_t i = 0; i < nf; i++) if (values[i]) east_value_release(values[i]);
                free(names); free(values);
                return NULL;
            } else {
                if (err) pe_set(err, strdup("expected ',' or ')' after struct field"),
                               sep->line, sep->column);
                for (size_t i = 0; i < nf; i++) if (values[i]) east_value_release(values[i]);
                free(names); free(values);
                return NULL;
            }
        }

        /* After loop, expect ')' */
        Token2 *close = ts2_cur(ts);
        if (close->type != TOK_RPAREN) {
            if (err) pe_set(err, strdup("expected ')' to close struct"),
                           close->line, close->column);
            for (size_t i = 0; i < nf; i++) if (values[i]) east_value_release(values[i]);
            free(names); free(values);
            return NULL;
        }
        ts2_adv(ts);

        EastValue *result = east_struct_new(names, values, nf, type);
        for (size_t i = 0; i < nf; i++) east_value_release(values[i]);
        free(names);
        free(values);
        return result;
    }

    case EAST_TYPE_VARIANT: {
        /* Expect '.' for variant */
        if (tok->type == TOK_DOT) {
            /* Whitespace between '.' and identifier */
            Token2 *next = ts2_cur(ts);
            /* DOT was already tokenized — next token after dot */
            ts2_adv(ts); /* consume DOT */
            if (err) pe_set(err, strdup("whitespace not allowed between '.' and case identifier"),
                           next->line, next->column + 1);
            return NULL;
        }
        if (tok->type != TOK_VARIANT_TAG) {
            if (err) pe_set(err, strdup("expected '.' to start variant case"),
                           tok->line, tok->column);
            return NULL;
        }
        ts2_adv(ts);

        const char *case_name = tok->text;
        EastType *case_type = NULL;
        for (size_t i = 0; i < type->data.variant.num_cases; i++) {
            if (strcmp(type->data.variant.cases[i].name, case_name) == 0) {
                case_type = type->data.variant.cases[i].type;
                break;
            }
        }
        if (!case_type) {
            if (err) {
                /* Build "unknown variant case .X, expected one of: .A, .B" */
                size_t elen = 60 + strlen(case_name);
                /* Sort case names alphabetically for consistent output */
                size_t nc = type->data.variant.num_cases;
                char **sorted = malloc(nc * sizeof(char *));
                for (size_t i = 0; i < nc; i++) sorted[i] = (char *)type->data.variant.cases[i].name;
                /* Simple bubble sort */
                for (size_t i = 0; i < nc; i++)
                    for (size_t j = i + 1; j < nc; j++)
                        if (strcmp(sorted[i], sorted[j]) > 0) {
                            char *tmp = sorted[i]; sorted[i] = sorted[j]; sorted[j] = tmp;
                        }
                for (size_t i = 0; i < nc; i++) elen += strlen(sorted[i]) + 3;
                char *msg = malloc(elen);
                int off = snprintf(msg, elen, "unknown variant case .%s, expected one of: ", case_name);
                for (size_t i = 0; i < nc; i++) {
                    if (i > 0) off += snprintf(msg + off, elen - off, ", ");
                    off += snprintf(msg + off, elen - off, ".%s", sorted[i]);
                }
                free(sorted);
                /* Position: column of the case name (after the dot) */
                pe_set(err, msg, tok->line, tok->column + 1);
            }
            return NULL;
        }

        EastValue *case_value;
        if (case_type->kind == EAST_TYPE_NULL) {
            /* Nullary variant: optionally accept explicit "null", but error on non-null data */
            Token2 *next = ts2_cur(ts);
            if (next->type == TOK_NULL_TOK) {
                ts2_adv(ts);
                case_value = east_null();
            } else if (next->type == TOK_EOF_TOK || next->type == TOK_COMMA ||
                       next->type == TOK_RPAREN || next->type == TOK_RBRACKET ||
                       next->type == TOK_RBRACE) {
                /* At a terminator — that's ok for optional null */
                case_value = east_null();
            } else {
                /* Non-null data for a null case */
                if (err) {
                    char *got = pe_got_token(next, input);
                    size_t len = 30 + strlen(got);
                    char *msg = malloc(len);
                    snprintf(msg, len, "expected null, got %s", got);
                    free(got);
                    pe_set(err, msg, next->line, next->column);
                    char path_buf[256];
                    snprintf(path_buf, sizeof(path_buf), ".%s", case_name);
                    pe_prepend_path(err, path_buf);
                }
                return NULL;
            }
        } else {
            char path_buf[256];
            snprintf(path_buf, sizeof(path_buf), ".%s", case_name);

            ParseErr inner = {0};
            case_value = parse_val_err(ts, case_type, ctx, err ? &inner : NULL, input);
            if (!case_value) {
                if (err && inner.message) {
                    pe_prepend_path(&inner, path_buf);
                    *err = inner;
                }
                return NULL;
            }
        }

        EastValue *result = east_variant_new(case_name, case_value, type);
        east_value_release(case_value);
        return result;
    }

    case EAST_TYPE_REF: {
        if (ctx && ts2_cur(ts)->type == TOK_BACKREF)
            return pctx_resolve_backref(ts, ctx);
        if (!ts2_match(ts, TOK_AMPERSAND)) return NULL;
        EastValue *inner = parse_val_err(ts, type->data.element, ctx, err, input);
        if (!inner) return NULL;
        EastValue *ref = east_ref_new(inner);
        east_value_release(inner);
        if (ctx) pctx_register(ctx, ref);
        return ref;
    }

    case EAST_TYPE_VECTOR: {
        EastType *elem_type = type->data.element;
        Token2 *cur = ts2_cur(ts);
        if (cur->type != TOK_IDENTIFIER || !cur->text || strcmp(cur->text, "vec") != 0)
            return NULL;
        ts2_adv(ts);
        if (!ts2_match(ts, TOK_LBRACKET)) return NULL;
        size_t cap = 16, vlen = 0;
        size_t elem_size = 0;
        if (elem_type->kind == EAST_TYPE_FLOAT) elem_size = sizeof(double);
        else if (elem_type->kind == EAST_TYPE_INTEGER) elem_size = sizeof(int64_t);
        else if (elem_type->kind == EAST_TYPE_BOOLEAN) elem_size = sizeof(bool);
        void *tmp = malloc(cap * elem_size);
        if (ts2_cur(ts)->type != TOK_RBRACKET) {
            for (;;) {
                EastValue *elem = parse_val_err(ts, elem_type, ctx, err, input);
                if (!elem) { free(tmp); return NULL; }
                if (vlen >= cap) { cap *= 2; tmp = realloc(tmp, cap * elem_size); }
                if (elem_type->kind == EAST_TYPE_FLOAT) ((double *)tmp)[vlen] = elem->data.float64;
                else if (elem_type->kind == EAST_TYPE_INTEGER) ((int64_t *)tmp)[vlen] = elem->data.integer;
                else if (elem_type->kind == EAST_TYPE_BOOLEAN) ((bool *)tmp)[vlen] = elem->data.boolean;
                east_value_release(elem);
                vlen++;
                if (!ts2_match(ts, TOK_COMMA)) break;
            }
        }
        if (!ts2_match(ts, TOK_RBRACKET)) { free(tmp); return NULL; }
        EastValue *vec = east_vector_new(elem_type, vlen);
        if (vec && vlen > 0) memcpy(vec->data.vector.data, tmp, vlen * elem_size);
        free(tmp);
        return vec;
    }

    case EAST_TYPE_MATRIX: {
        EastType *elem_type = type->data.element;
        Token2 *cur = ts2_cur(ts);
        if (cur->type != TOK_IDENTIFIER || !cur->text || strcmp(cur->text, "mat") != 0)
            return NULL;
        ts2_adv(ts);
        if (!ts2_match(ts, TOK_LBRACKET)) return NULL;
        size_t rows = 0, cols = 0, cap_flat = 64;
        size_t elem_size = 0;
        if (elem_type->kind == EAST_TYPE_FLOAT) elem_size = sizeof(double);
        else if (elem_type->kind == EAST_TYPE_INTEGER) elem_size = sizeof(int64_t);
        else if (elem_type->kind == EAST_TYPE_BOOLEAN) elem_size = sizeof(bool);
        void *flat = malloc(cap_flat * elem_size);
        size_t flat_len = 0;
        if (ts2_cur(ts)->type != TOK_RBRACKET) {
            for (;;) {
                if (!ts2_match(ts, TOK_LBRACKET)) { free(flat); return NULL; }
                size_t row_cols = 0;
                if (ts2_cur(ts)->type != TOK_RBRACKET) {
                    for (;;) {
                        if (flat_len >= cap_flat) { cap_flat *= 2; flat = realloc(flat, cap_flat * elem_size); }
                        EastValue *elem = parse_val_err(ts, elem_type, ctx, err, input);
                        if (!elem) { free(flat); return NULL; }
                        if (elem_type->kind == EAST_TYPE_FLOAT) ((double *)flat)[flat_len] = elem->data.float64;
                        else if (elem_type->kind == EAST_TYPE_INTEGER) ((int64_t *)flat)[flat_len] = elem->data.integer;
                        else if (elem_type->kind == EAST_TYPE_BOOLEAN) ((bool *)flat)[flat_len] = elem->data.boolean;
                        east_value_release(elem);
                        flat_len++; row_cols++;
                        if (!ts2_match(ts, TOK_COMMA)) break;
                    }
                }
                if (!ts2_match(ts, TOK_RBRACKET)) { free(flat); return NULL; }
                if (rows == 0) cols = row_cols;
                rows++;
                if (!ts2_match(ts, TOK_COMMA)) break;
            }
        }
        if (!ts2_match(ts, TOK_RBRACKET)) { free(flat); return NULL; }
        EastValue *mat = east_matrix_new(elem_type, rows, cols);
        if (mat && flat_len > 0) memcpy(mat->data.matrix.data, flat, flat_len * elem_size);
        free(flat);
        return mat;
    }

    case EAST_TYPE_RECURSIVE:
        if (type->data.recursive.node)
            return parse_val_err(ts, type->data.recursive.node, ctx, err, input);
        return NULL;

    case EAST_TYPE_NEVER:
    case EAST_TYPE_FUNCTION:
    case EAST_TYPE_ASYNC_FUNCTION:
        return NULL;
    }
    return NULL;
}

/* ================================================================== */
/*  Public API: east_parse_value_with_error                            */
/* ================================================================== */

EastValue *east_parse_value_with_error(const char *text, EastType *type, char **error_out)
{
    if (!text || !type) return NULL;
    TokStream2 ts = ts2_new(text);
    ParseContext ctx = {0};
    ParseErr err;
    pe_init(&err);

    EastValue *result = parse_val_err(&ts, type, &ctx, error_out ? &err : NULL, text);

    if (result && ts2_cur(&ts)->type != TOK_EOF_TOK) {
        /* Unexpected trailing input */
        Token2 *extra = ts2_cur(&ts);
        if (error_out) {
            char *type_str = east_print_type(type);
            size_t total = 200 + (type_str ? strlen(type_str) : 0);
            char *full_msg = malloc(total);
            snprintf(full_msg, total,
                "Error occurred because unexpected input after parsed value (line %d, col %d) while parsing value of type \"%s\"",
                extra->line, extra->column, type_str ? type_str : "?");
            free(type_str);
            *error_out = full_msg;
        }
        east_value_release(result);
        result = NULL;
    } else if (!result && error_out && err.message) {
        char *type_str = east_print_type(type);
        const char *path_str = (err.path && err.path[0]) ? err.path : NULL;
        size_t total = strlen(err.message) + 200;
        if (path_str) total += strlen(path_str);
        if (type_str) total += strlen(type_str);
        char *full_msg = malloc(total);
        if (path_str) {
            snprintf(full_msg, total,
                "Error occurred because %s at %s (line %d, col %d) while parsing value of type \"%s\"",
                err.message, path_str, err.line, err.column,
                type_str ? type_str : "?");
        } else {
            snprintf(full_msg, total,
                "Error occurred because %s (line %d, col %d) while parsing value of type \"%s\"",
                err.message, err.line, err.column,
                type_str ? type_str : "?");
        }
        free(type_str);
        *error_out = full_msg;
    } else if (error_out) {
        *error_out = NULL;
    }

    pe_free(&err);
    pctx_free(&ctx);
    ts2_free(&ts);
    return result;
}

/* ================================================================== */
/*  Type parser                                                        */
/* ================================================================== */

static EastType *parse_type_internal(TokStream2 *ts);

static EastType *parse_type_internal(TokStream2 *ts)
{
    Token2 *tok = ts2_cur(ts);

    /* Types in East text format start with a variant tag:
     * .Null, .Boolean, .Integer, .Float, .String, .DateTime, .Blob,
     * .Array <elem>, .Set <elem>, .Dict (key=..., value=...),
     * .Struct [...], .Variant [...], .Ref <inner>,
     * .Vector <elem>, .Matrix <elem>,
     * .Function (inputs=[...], output=...), .Recursive <depth>
     * .Never
     */
    if (tok->type != TOK_VARIANT_TAG) return NULL;
    const char *tag = tok->text;
    ts2_adv(ts);

    if (strcmp(tag, "Never") == 0) return &east_never_type;
    if (strcmp(tag, "Null") == 0) return &east_null_type;
    if (strcmp(tag, "Boolean") == 0) return &east_boolean_type;
    if (strcmp(tag, "Integer") == 0) return &east_integer_type;
    if (strcmp(tag, "Float") == 0) return &east_float_type;
    if (strcmp(tag, "String") == 0) return &east_string_type;
    if (strcmp(tag, "DateTime") == 0) return &east_datetime_type;
    if (strcmp(tag, "Blob") == 0) return &east_blob_type;

    if (strcmp(tag, "Array") == 0) {
        EastType *elem = parse_type_internal(ts);
        if (!elem) return NULL;
        EastType *t = east_array_type(elem);
        east_type_release(elem);
        return t;
    }

    if (strcmp(tag, "Set") == 0) {
        EastType *elem = parse_type_internal(ts);
        if (!elem) return NULL;
        EastType *t = east_set_type(elem);
        east_type_release(elem);
        return t;
    }

    if (strcmp(tag, "Vector") == 0) {
        EastType *elem = parse_type_internal(ts);
        if (!elem) return NULL;
        EastType *t = east_vector_type(elem);
        east_type_release(elem);
        return t;
    }

    if (strcmp(tag, "Matrix") == 0) {
        EastType *elem = parse_type_internal(ts);
        if (!elem) return NULL;
        EastType *t = east_matrix_type(elem);
        east_type_release(elem);
        return t;
    }

    if (strcmp(tag, "Ref") == 0) {
        EastType *elem = parse_type_internal(ts);
        if (!elem) return NULL;
        EastType *t = east_ref_type(elem);
        east_type_release(elem);
        return t;
    }

    if (strcmp(tag, "Dict") == 0) {
        /* .Dict (key=<type>, value=<type>) */
        if (!ts2_match(ts, TOK_LPAREN)) return NULL;

        EastType *key = NULL;
        EastType *val = NULL;

        for (int i = 0; i < 2; i++) {
            Token2 *name = ts2_cur(ts);
            if (name->type != TOK_IDENTIFIER) break;
            ts2_adv(ts);
            if (!ts2_match(ts, TOK_EQUALS)) break;

            if (strcmp(name->text, "key") == 0) {
                key = parse_type_internal(ts);
            } else if (strcmp(name->text, "value") == 0) {
                val = parse_type_internal(ts);
            }
            ts2_match(ts, TOK_COMMA);
        }

        ts2_match(ts, TOK_RPAREN);

        if (!key || !val) {
            if (key) east_type_release(key);
            if (val) east_type_release(val);
            return NULL;
        }
        EastType *t = east_dict_type(key, val);
        east_type_release(key);
        east_type_release(val);
        return t;
    }

    if (strcmp(tag, "Struct") == 0) {
        /* .Struct [(name="field1", type=<type>), ...] */
        if (!ts2_match(ts, TOK_LBRACKET)) return NULL;

        size_t cap = 16, count = 0;
        const char **names = malloc(cap * sizeof(char *));
        EastType **types = malloc(cap * sizeof(EastType *));

        if (ts2_cur(ts)->type != TOK_RBRACKET) {
            for (;;) {
                if (!ts2_match(ts, TOK_LPAREN)) break;

                char *fname = NULL;
                EastType *ftype = NULL;

                for (int fi = 0; fi < 2; fi++) {
                    Token2 *n = ts2_cur(ts);
                    if (n->type != TOK_IDENTIFIER) break;
                    ts2_adv(ts);
                    ts2_match(ts, TOK_EQUALS);

                    if (strcmp(n->text, "name") == 0) {
                        Token2 *s = ts2_cur(ts);
                        if (s->type == TOK_STRING) {
                            fname = strdup(s->text);
                            ts2_adv(ts);
                        }
                    } else if (strcmp(n->text, "type") == 0) {
                        ftype = parse_type_internal(ts);
                    }
                    ts2_match(ts, TOK_COMMA);
                }

                ts2_match(ts, TOK_RPAREN);

                if (fname && ftype) {
                    if (count >= cap) {
                        cap *= 2;
                        names = realloc(names, cap * sizeof(char *));
                        types = realloc(types, cap * sizeof(EastType *));
                    }
                    names[count] = fname;
                    types[count] = ftype;
                    count++;
                } else {
                    free(fname);
                    if (ftype) east_type_release(ftype);
                }

                if (!ts2_match(ts, TOK_COMMA)) break;
            }
        }
        ts2_match(ts, TOK_RBRACKET);

        EastType *t = east_struct_type(names, types, count);
        for (size_t i = 0; i < count; i++) {
            free((char *)names[i]);
            east_type_release(types[i]);
        }
        free(names);
        free(types);
        return t;
    }

    if (strcmp(tag, "Variant") == 0) {
        /* .Variant [(name="case1", type=<type>), ...] */
        if (!ts2_match(ts, TOK_LBRACKET)) return NULL;

        size_t cap = 16, count = 0;
        const char **names = malloc(cap * sizeof(char *));
        EastType **types = malloc(cap * sizeof(EastType *));

        if (ts2_cur(ts)->type != TOK_RBRACKET) {
            for (;;) {
                if (!ts2_match(ts, TOK_LPAREN)) break;

                char *cname = NULL;
                EastType *ctype = NULL;

                for (int fi = 0; fi < 2; fi++) {
                    Token2 *n = ts2_cur(ts);
                    if (n->type != TOK_IDENTIFIER) break;
                    ts2_adv(ts);
                    ts2_match(ts, TOK_EQUALS);

                    if (strcmp(n->text, "name") == 0) {
                        Token2 *s = ts2_cur(ts);
                        if (s->type == TOK_STRING) {
                            cname = strdup(s->text);
                            ts2_adv(ts);
                        }
                    } else if (strcmp(n->text, "type") == 0) {
                        ctype = parse_type_internal(ts);
                    }
                    ts2_match(ts, TOK_COMMA);
                }

                ts2_match(ts, TOK_RPAREN);

                if (cname && ctype) {
                    if (count >= cap) {
                        cap *= 2;
                        names = realloc(names, cap * sizeof(char *));
                        types = realloc(types, cap * sizeof(EastType *));
                    }
                    names[count] = cname;
                    types[count] = ctype;
                    count++;
                } else {
                    free(cname);
                    if (ctype) east_type_release(ctype);
                }

                if (!ts2_match(ts, TOK_COMMA)) break;
            }
        }
        ts2_match(ts, TOK_RBRACKET);

        EastType *t = east_variant_type(names, types, count);
        for (size_t i = 0; i < count; i++) {
            free((char *)names[i]);
            east_type_release(types[i]);
        }
        free(names);
        free(types);
        return t;
    }

    if (strcmp(tag, "Function") == 0 || strcmp(tag, "AsyncFunction") == 0) {
        bool is_async = (strcmp(tag, "AsyncFunction") == 0);

        /* .Function (inputs=[...], output=<type>) */
        if (!ts2_match(ts, TOK_LPAREN)) return NULL;

        EastType **inputs = NULL;
        size_t num_inputs = 0;
        EastType *output = NULL;

        for (int fi = 0; fi < 2; fi++) {
            Token2 *n = ts2_cur(ts);
            if (n->type != TOK_IDENTIFIER) break;
            ts2_adv(ts);
            ts2_match(ts, TOK_EQUALS);

            if (strcmp(n->text, "inputs") == 0) {
                ts2_match(ts, TOK_LBRACKET);
                size_t icap = 8;
                inputs = malloc(icap * sizeof(EastType *));
                if (ts2_cur(ts)->type != TOK_RBRACKET) {
                    for (;;) {
                        EastType *inp = parse_type_internal(ts);
                        if (!inp) break;
                        if (num_inputs >= icap) {
                            icap *= 2;
                            inputs = realloc(inputs, icap * sizeof(EastType *));
                        }
                        inputs[num_inputs++] = inp;
                        if (!ts2_match(ts, TOK_COMMA)) break;
                    }
                }
                ts2_match(ts, TOK_RBRACKET);
            } else if (strcmp(n->text, "output") == 0) {
                output = parse_type_internal(ts);
            }
            ts2_match(ts, TOK_COMMA);
        }

        ts2_match(ts, TOK_RPAREN);

        if (!output) output = &east_null_type;

        EastType *t;
        if (is_async) {
            t = east_async_function_type(inputs, num_inputs, output);
        } else {
            t = east_function_type(inputs, num_inputs, output);
        }
        for (size_t i = 0; i < num_inputs; i++) {
            east_type_release(inputs[i]);
        }
        free(inputs);
        if (output != &east_null_type) east_type_release(output);
        return t;
    }

    if (strcmp(tag, "Recursive") == 0) {
        /* Recursive types cannot be fully reconstituted from text alone
         * because they require circular references. Create an empty wrapper. */
        return east_recursive_type_new();
    }

    return NULL;
}

EastType *east_parse_type(const char *text)
{
    if (!text) return NULL;
    TokStream2 ts = ts2_new(text);
    EastType *result = parse_type_internal(&ts);
    ts2_free(&ts);
    return result;
}
