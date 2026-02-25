/*
 * Tokenizer for East text format.
 *
 * Breaks East text into tokens for the parser.
 * Supports: null, true, false, integers, floats, strings, blobs, datetimes,
 * identifiers, variant tags (.Tag), and delimiters.
 *
 * Token types:
 *   STRING, INTEGER, FLOAT, TRUE, FALSE, NULL_TOK,
 *   DOT, COLON, COMMA, LBRACKET, RBRACKET, LBRACE, RBRACE,
 *   LPAREN, RPAREN, EQUALS, AMPERSAND, PIPE,
 *   HEX (blob literal), DATETIME_LIT, IDENTIFIER, VARIANT_TAG,
 *   EOF_TOK
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
/*  Token types                                                        */
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
    TOK_EOF_TOK,
} EastTokenType;

typedef struct {
    EastTokenType type;
    char *text;           /* allocated string for the token text */
    size_t text_len;
    int64_t int_val;      /* for INTEGER tokens */
    double float_val;     /* for FLOAT tokens */
    int line;
    int column;
} EastToken;

typedef struct {
    EastToken *tokens;
    size_t count;
    size_t cap;
} TokenArray;

/* ================================================================== */
/*  TokenArray helpers                                                  */
/* ================================================================== */

static TokenArray token_array_new(void)
{
    TokenArray ta;
    ta.cap = 64;
    ta.tokens = calloc(ta.cap, sizeof(EastToken));
    ta.count = 0;
    return ta;
}

static void token_array_push(TokenArray *ta, EastToken tok)
{
    if (ta->count >= ta->cap) {
        ta->cap *= 2;
        ta->tokens = realloc(ta->tokens, ta->cap * sizeof(EastToken));
    }
    ta->tokens[ta->count++] = tok;
}

static void token_array_free(TokenArray *ta)
{
    for (size_t i = 0; i < ta->count; i++) {
        free(ta->tokens[i].text);
    }
    free(ta->tokens);
    ta->tokens = NULL;
    ta->count = 0;
}

/* ================================================================== */
/*  Tokenizer state                                                    */
/* ================================================================== */

typedef struct {
    const char *text;
    size_t len;
    size_t pos;
    int line;
    int column;
} Tokenizer;

static Tokenizer tokenizer_new(const char *text)
{
    Tokenizer t;
    t.text = text;
    t.len = strlen(text);
    t.pos = 0;
    t.line = 1;
    t.column = 1;
    return t;
}

static char tok_current(Tokenizer *t)
{
    if (t->pos >= t->len) return '\0';
    return t->text[t->pos];
}

static char tok_peek(Tokenizer *t, int offset)
{
    size_t p = t->pos + (size_t)offset;
    if (p >= t->len) return '\0';
    return t->text[p];
}

static char tok_advance(Tokenizer *t)
{
    if (t->pos >= t->len) return '\0';
    char c = t->text[t->pos++];
    if (c == '\n') {
        t->line++;
        t->column = 1;
    } else {
        t->column++;
    }
    return c;
}

static void tok_skip_whitespace(Tokenizer *t)
{
    while (t->pos < t->len) {
        char c = t->text[t->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            tok_advance(t);
            continue;
        }
        /* Skip # comments */
        if (c == '#') {
            while (t->pos < t->len && t->text[t->pos] != '\n') {
                tok_advance(t);
            }
            continue;
        }
        break;
    }
}

/* ================================================================== */
/*  Read a string literal                                              */
/* ================================================================== */

static char *tok_read_string(Tokenizer *t, size_t *out_len)
{
    char quote = tok_advance(t); /* consume opening quote */
    size_t cap = 64;
    size_t len = 0;
    char *buf = malloc(cap);

    while (t->pos < t->len) {
        char c = tok_current(t);
        if (c == quote) {
            tok_advance(t); /* consume closing quote */
            if (out_len) *out_len = len;
            buf[len] = '\0';
            return buf;
        }
        if (c == '\\') {
            tok_advance(t); /* skip backslash */
            char next = tok_current(t);
            if (next == '\\') { buf[len++] = '\\'; tok_advance(t); }
            else if (next == quote) { buf[len++] = quote; tok_advance(t); }
            else if (next == 'n') { buf[len++] = '\n'; tok_advance(t); }
            else if (next == 't') { buf[len++] = '\t'; tok_advance(t); }
            else if (next == 'r') { buf[len++] = '\r'; tok_advance(t); }
            else { buf[len++] = next; tok_advance(t); }
        } else {
            buf[len++] = c;
            tok_advance(t);
        }
        if (len >= cap - 1) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
    }

    /* Unterminated string */
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

/* ================================================================== */
/*  Read a number or datetime                                          */
/* ================================================================== */

static EastToken tok_read_number_or_datetime(Tokenizer *t)
{
    int start_line = t->line;
    int start_col = t->column;

    /* Check for -Infinity */
    if (tok_current(t) == '-' && t->pos + 9 <= t->len &&
        memcmp(t->text + t->pos + 1, "Infinity", 8) == 0) {
        for (int i = 0; i < 9; i++) tok_advance(t);
        EastToken tok = {0};
        tok.type = TOK_FLOAT;
        tok.float_val = -INFINITY;
        tok.text = strdup("-Infinity");
        tok.text_len = 9;
        tok.line = start_line;
        tok.column = start_col;
        return tok;
    }

    /* Collect characters that could be part of number or datetime */
    size_t cap = 64;
    size_t len = 0;
    char *buf = malloc(cap);
    bool has_t_sep = false;

    while (t->pos < t->len) {
        char c = tok_current(t);
        if (c == ':') {
            /* Only include colon if we've seen T or dash (indicating datetime) */
            if (has_t_sep || memchr(buf, '-', len)) {
                buf[len++] = c;
                tok_advance(t);
            } else {
                break;
            }
        } else if (isdigit((unsigned char)c) ||
                   c == '+' || c == '-' || c == '.' || c == 'T' ||
                   c == 'Z' || c == 'e' || c == 'E') {
            if (c == 'T') has_t_sep = true;
            buf[len++] = c;
            tok_advance(t);
        } else {
            break;
        }
        if (len >= cap - 1) { cap *= 2; buf = realloc(buf, cap); }
    }
    buf[len] = '\0';

    EastToken tok = {0};
    tok.line = start_line;
    tok.column = start_col;
    tok.text = buf;
    tok.text_len = len;

    /* Check for datetime */
    if (has_t_sep || (memchr(buf, ':', len) && memchr(buf, '-', len))) {
        tok.type = TOK_DATETIME_LIT;
        return tok;
    }

    /* Check for float */
    if (memchr(buf, '.', len) || memchr(buf, 'e', len) || memchr(buf, 'E', len)) {
        tok.type = TOK_FLOAT;
        tok.float_val = strtod(buf, NULL);
        return tok;
    }

    /* Integer */
    tok.type = TOK_INTEGER;
    tok.int_val = strtoll(buf, NULL, 10);
    return tok;
}

/* ================================================================== */
/*  Read an identifier or keyword                                      */
/* ================================================================== */

static EastToken tok_read_identifier(Tokenizer *t)
{
    int start_line = t->line;
    int start_col = t->column;

    /* Check for backtick-escaped identifier */
    if (tok_current(t) == '`') {
        tok_advance(t);
        size_t cap = 64, len = 0;
        char *buf = malloc(cap);
        while (t->pos < t->len && tok_current(t) != '`') {
            buf[len++] = tok_current(t);
            tok_advance(t);
            if (len >= cap - 1) { cap *= 2; buf = realloc(buf, cap); }
        }
        if (tok_current(t) == '`') tok_advance(t);
        buf[len] = '\0';

        EastToken tok = {0};
        tok.type = TOK_IDENTIFIER;
        tok.text = buf;
        tok.text_len = len;
        tok.line = start_line;
        tok.column = start_col;
        return tok;
    }

    size_t cap = 64, len = 0;
    char *buf = malloc(cap);

    while (t->pos < t->len) {
        char c = tok_current(t);
        if (isalnum((unsigned char)c) || c == '_') {
            buf[len++] = c;
            tok_advance(t);
        } else {
            break;
        }
        if (len >= cap - 1) { cap *= 2; buf = realloc(buf, cap); }
    }
    buf[len] = '\0';

    EastToken tok = {0};
    tok.text = buf;
    tok.text_len = len;
    tok.line = start_line;
    tok.column = start_col;

    /* Check keywords */
    if (strcmp(buf, "null") == 0) {
        tok.type = TOK_NULL_TOK;
    } else if (strcmp(buf, "true") == 0) {
        tok.type = TOK_TRUE;
    } else if (strcmp(buf, "false") == 0) {
        tok.type = TOK_FALSE;
    } else if (strcmp(buf, "NaN") == 0) {
        tok.type = TOK_FLOAT;
        tok.float_val = NAN;
    } else if (strcmp(buf, "Infinity") == 0) {
        tok.type = TOK_FLOAT;
        tok.float_val = INFINITY;
    } else {
        tok.type = TOK_IDENTIFIER;
    }

    return tok;
}

/* ================================================================== */
/*  Read a blob literal (0x...)                                        */
/* ================================================================== */

static EastToken tok_read_blob(Tokenizer *t)
{
    int start_line = t->line;
    int start_col = t->column;

    tok_advance(t); /* '0' */
    tok_advance(t); /* 'x' */

    size_t cap = 64, len = 0;
    char *buf = malloc(cap);

    while (t->pos < t->len) {
        char c = tok_current(t);
        if (isxdigit((unsigned char)c)) {
            buf[len++] = c;
            tok_advance(t);
        } else {
            break;
        }
        if (len >= cap - 1) { cap *= 2; buf = realloc(buf, cap); }
    }
    buf[len] = '\0';

    EastToken tok = {0};
    tok.type = TOK_HEX;
    tok.text = buf;
    tok.text_len = len;
    tok.line = start_line;
    tok.column = start_col;
    return tok;
}

/* ================================================================== */
/*  Main tokenize function                                             */
/* ================================================================== */

static TokenArray east_tokenize(const char *text)
{
    Tokenizer t = tokenizer_new(text);
    TokenArray result = token_array_new();

    while (1) {
        tok_skip_whitespace(&t);

        if (t.pos >= t.len) {
            EastToken eof = {0};
            eof.type = TOK_EOF_TOK;
            eof.line = t.line;
            eof.column = t.column;
            token_array_push(&result, eof);
            break;
        }

        char c = tok_current(&t);
        int start_line = t.line;
        int start_col = t.column;

        /* Single-character delimiters */
        if (c == '[') {
            tok_advance(&t);
            EastToken tok = {0};
            tok.type = TOK_LBRACKET;
            tok.line = start_line; tok.column = start_col;
            token_array_push(&result, tok);
        } else if (c == ']') {
            tok_advance(&t);
            EastToken tok = {0};
            tok.type = TOK_RBRACKET;
            tok.line = start_line; tok.column = start_col;
            token_array_push(&result, tok);
        } else if (c == '{') {
            tok_advance(&t);
            EastToken tok = {0};
            tok.type = TOK_LBRACE;
            tok.line = start_line; tok.column = start_col;
            token_array_push(&result, tok);
        } else if (c == '}') {
            tok_advance(&t);
            EastToken tok = {0};
            tok.type = TOK_RBRACE;
            tok.line = start_line; tok.column = start_col;
            token_array_push(&result, tok);
        } else if (c == '(') {
            tok_advance(&t);
            EastToken tok = {0};
            tok.type = TOK_LPAREN;
            tok.line = start_line; tok.column = start_col;
            token_array_push(&result, tok);
        } else if (c == ')') {
            tok_advance(&t);
            EastToken tok = {0};
            tok.type = TOK_RPAREN;
            tok.line = start_line; tok.column = start_col;
            token_array_push(&result, tok);
        } else if (c == ',') {
            tok_advance(&t);
            EastToken tok = {0};
            tok.type = TOK_COMMA;
            tok.line = start_line; tok.column = start_col;
            token_array_push(&result, tok);
        } else if (c == ':') {
            tok_advance(&t);
            EastToken tok = {0};
            tok.type = TOK_COLON;
            tok.line = start_line; tok.column = start_col;
            token_array_push(&result, tok);
        } else if (c == '=') {
            tok_advance(&t);
            EastToken tok = {0};
            tok.type = TOK_EQUALS;
            tok.line = start_line; tok.column = start_col;
            token_array_push(&result, tok);
        } else if (c == '&') {
            tok_advance(&t);
            EastToken tok = {0};
            tok.type = TOK_AMPERSAND;
            tok.line = start_line; tok.column = start_col;
            token_array_push(&result, tok);
        } else if (c == '|') {
            tok_advance(&t);
            EastToken tok = {0};
            tok.type = TOK_PIPE;
            tok.line = start_line; tok.column = start_col;
            token_array_push(&result, tok);

        /* Variant tag (.Identifier) */
        } else if (c == '.') {
            tok_advance(&t);
            char next = tok_current(&t);
            if (isalpha((unsigned char)next) || next == '_') {
                /* Read tag name */
                size_t cap = 64, len = 0;
                char *buf = malloc(cap);
                while (t.pos < t.len) {
                    char cc = tok_current(&t);
                    if (isalnum((unsigned char)cc) || cc == '_') {
                        buf[len++] = cc;
                        tok_advance(&t);
                    } else {
                        break;
                    }
                    if (len >= cap - 1) { cap *= 2; buf = realloc(buf, cap); }
                }
                buf[len] = '\0';

                EastToken tok = {0};
                tok.type = TOK_VARIANT_TAG;
                tok.text = buf;
                tok.text_len = len;
                tok.line = start_line;
                tok.column = start_col;
                token_array_push(&result, tok);
            } else {
                EastToken tok = {0};
                tok.type = TOK_DOT;
                tok.line = start_line; tok.column = start_col;
                token_array_push(&result, tok);
            }

        /* String literals */
        } else if (c == '"' || c == '\'') {
            size_t slen;
            char *str = tok_read_string(&t, &slen);
            EastToken tok = {0};
            tok.type = TOK_STRING;
            tok.text = str;
            tok.text_len = slen;
            tok.line = start_line;
            tok.column = start_col;
            token_array_push(&result, tok);

        /* Blob literal (0x...) */
        } else if (c == '0' && tok_peek(&t, 1) == 'x') {
            token_array_push(&result, tok_read_blob(&t));

        /* Number or datetime */
        } else if (isdigit((unsigned char)c)) {
            token_array_push(&result, tok_read_number_or_datetime(&t));

        /* Negative number or -Infinity */
        } else if (c == '-') {
            char next = tok_peek(&t, 1);
            if (isdigit((unsigned char)next) || next == 'I') {
                token_array_push(&result, tok_read_number_or_datetime(&t));
            } else {
                /* Unexpected */
                tok_advance(&t);
            }

        /* Identifiers and keywords */
        } else if (isalpha((unsigned char)c) || c == '_' || c == '`') {
            token_array_push(&result, tok_read_identifier(&t));

        } else {
            /* Skip unrecognized character */
            tok_advance(&t);
        }
    }

    return result;
}

/* ================================================================== */
/*  Token stream for parser consumption                                */
/* ================================================================== */

typedef struct {
    TokenArray ta;
    size_t pos;
} EastTokenStream;

static EastTokenStream token_stream_new(const char *text)
{
    EastTokenStream ts;
    ts.ta = east_tokenize(text);
    ts.pos = 0;
    return ts;
}

static void token_stream_free(EastTokenStream *ts)
{
    token_array_free(&ts->ta);
}

static EastToken *ts_current(EastTokenStream *ts)
{
    if (ts->pos >= ts->ta.count) return &ts->ta.tokens[ts->ta.count - 1];
    return &ts->ta.tokens[ts->pos];
}

static EastToken *ts_advance(EastTokenStream *ts)
{
    EastToken *tok = ts_current(ts);
    if (ts->pos < ts->ta.count - 1) ts->pos++;
    return tok;
}

static bool ts_match(EastTokenStream *ts, EastTokenType type)
{
    if (ts_current(ts)->type == type) {
        ts_advance(ts);
        return true;
    }
    return false;
}

static EastToken *ts_expect(EastTokenStream *ts, EastTokenType type)
{
    EastToken *tok = ts_current(ts);
    if (tok->type != type) return NULL;
    return ts_advance(ts);
}

/* Make these accessible to east_parser.c via internal declarations */
/* (In a real build system, these would be in an internal header) */
