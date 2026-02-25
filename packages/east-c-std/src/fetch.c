/*
 * HTTP fetch platform functions for East.
 *
 * Provides HTTP request operations for East programs running in C.
 * Conditional on EAST_HAS_CURL -- if libcurl is not available, all
 * fetch functions return error values.
 */

#include "east_std/east_std.h"
#include <east/values.h>
#include <east/eval_result.h>
#include <east/types.h>
#include <stdlib.h>
#include <string.h>

#include <stdio.h>

#ifdef EAST_HAS_CURL
#define CURL_DISABLE_TYPECHECK
#include <curl/curl.h>

/* ========================================================================
 * cURL helper: dynamic write buffer
 * ======================================================================== */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} WriteBuffer;

static void write_buffer_init(WriteBuffer *wb) {
    wb->data = malloc(1024);
    wb->len = 0;
    wb->cap = wb->data ? 1024 : 0;
}

static void write_buffer_free(WriteBuffer *wb) {
    free(wb->data);
    wb->data = NULL;
    wb->len = 0;
    wb->cap = 0;
}

static size_t east_curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    WriteBuffer *wb = (WriteBuffer *)userdata;
    size_t total = size * nmemb;

    if (wb->len + total >= wb->cap) {
        size_t new_cap = (wb->cap + total) * 2;
        char *new_data = realloc(wb->data, new_cap);
        if (!new_data) return 0;
        wb->data = new_data;
        wb->cap = new_cap;
    }

    memcpy(wb->data + wb->len, ptr, total);
    wb->len += total;
    return total;
}

/* ========================================================================
 * fetch_get: HTTP GET, return response body as string
 * ======================================================================== */

static EvalResult fetch_get(EastValue **args, size_t num_args) {
    (void)num_args;
    const char *url = args[0]->data.string.data;

    CURL *curl = curl_easy_init();
    if (!curl) {
        return eval_ok(east_string(""));
    }

    WriteBuffer wb;
    write_buffer_init(&wb);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, east_curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wb);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || !wb.data) {
        write_buffer_free(&wb);
        return eval_ok(east_string(""));
    }

    EastValue *result = east_string_len(wb.data, wb.len);
    write_buffer_free(&wb);
    return eval_ok(result);
}

/* ========================================================================
 * fetch_get_bytes: HTTP GET, return response body as blob
 * ======================================================================== */

static EvalResult fetch_get_bytes(EastValue **args, size_t num_args) {
    (void)num_args;
    const char *url = args[0]->data.string.data;

    CURL *curl = curl_easy_init();
    if (!curl) {
        return eval_ok(east_blob(NULL, 0));
    }

    WriteBuffer wb;
    write_buffer_init(&wb);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, east_curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wb);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || !wb.data) {
        write_buffer_free(&wb);
        return eval_ok(east_blob(NULL, 0));
    }

    EastValue *result = east_blob((const uint8_t *)wb.data, wb.len);
    write_buffer_free(&wb);
    return eval_ok(result);
}

/* ========================================================================
 * fetch_post: HTTP POST, return response body as string
 * ======================================================================== */

static EvalResult fetch_post(EastValue **args, size_t num_args) {
    (void)num_args;
    const char *url = args[0]->data.string.data;
    const char *body = args[1]->data.string.data;
    size_t body_len = args[1]->data.string.len;

    CURL *curl = curl_easy_init();
    if (!curl) {
        return eval_ok(east_string(""));
    }

    WriteBuffer wb;
    write_buffer_init(&wb);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: text/plain");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, east_curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wb);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || !wb.data) {
        write_buffer_free(&wb);
        return eval_ok(east_string(""));
    }

    EastValue *result = east_string_len(wb.data, wb.len);
    write_buffer_free(&wb);
    return eval_ok(result);
}

/* ========================================================================
 * fetch_request: full HTTP request with method/headers/body, returns struct
 * ======================================================================== */

/* Callback to capture response headers into a Dict<String, String> */
static size_t east_curl_header_cb(char *buffer, size_t size, size_t nitems,
                                  void *userdata) {
    EastValue *dict = (EastValue *)userdata;
    size_t total = size * nitems;

    /* Find colon separator */
    char *colon = memchr(buffer, ':', total);
    if (!colon) return total; /* skip status line and empty lines */

    size_t key_len = (size_t)(colon - buffer);
    char *val_start = colon + 1;
    size_t val_len = total - key_len - 1;

    /* Trim leading whitespace from value */
    while (val_len > 0 && *val_start == ' ') { val_start++; val_len--; }
    /* Trim trailing \r\n */
    while (val_len > 0 && (val_start[val_len - 1] == '\r' ||
                           val_start[val_len - 1] == '\n')) { val_len--; }

    /* Lowercase the key (HTTP headers are case-insensitive) */
    char *lkey = malloc(key_len + 1);
    if (!lkey) return total;
    for (size_t i = 0; i < key_len; i++)
        lkey[i] = (buffer[i] >= 'A' && buffer[i] <= 'Z')
                  ? buffer[i] + 32 : buffer[i];
    lkey[key_len] = '\0';

    EastValue *k = east_string_len(lkey, key_len);
    EastValue *v = east_string_len(val_start, val_len);
    east_dict_set(dict, k, v);
    east_value_release(k);
    east_value_release(v);
    free(lkey);
    return total;
}

static EvalResult fetch_request(EastValue **args, size_t num_args) {
    (void)num_args;
    EastValue *config = args[0];

    /* Extract fields from the config struct */
    EastValue *url_val = east_struct_get_field(config, "url");
    EastValue *method_val = east_struct_get_field(config, "method");
    EastValue *headers_val = east_struct_get_field(config, "headers");
    EastValue *body_val = east_struct_get_field(config, "body");

    const char *url = url_val->data.string.data;
    const char *method = method_val->data.variant.case_name;

    CURL *curl = curl_easy_init();
    if (!curl) {
        const char *field_names[] = { "status", "statusText", "headers", "body", "ok" };
        EastValue *field_values[5];
        field_values[0] = east_integer(0);
        field_values[1] = east_string("curl init failed");
        field_values[2] = east_dict_new(&east_string_type, &east_string_type);
        field_values[3] = east_string("");
        field_values[4] = east_boolean(false);
        return eval_ok(east_struct_new(field_names, field_values, 5, NULL));
    }

    WriteBuffer wb;
    write_buffer_init(&wb);

    /* Response headers dict */
    EastValue *resp_headers = east_dict_new(&east_string_type, &east_string_type);

    /* Set URL */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    /* Set HTTP method */
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);

    /* Set request headers from dict */
    struct curl_slist *curl_headers = NULL;
    if (headers_val && headers_val->kind == EAST_VAL_DICT) {
        size_t hdr_count = east_dict_len(headers_val);
        for (size_t i = 0; i < hdr_count; i++) {
            const char *key = headers_val->data.dict.keys[i]->data.string.data;
            const char *val = headers_val->data.dict.values[i]->data.string.data;
            size_t hdr_len = strlen(key) + strlen(val) + 3;
            char *hdr_line = malloc(hdr_len);
            if (hdr_line) {
                snprintf(hdr_line, hdr_len, "%s: %s", key, val);
                curl_headers = curl_slist_append(curl_headers, hdr_line);
                free(hdr_line);
            }
        }
        if (curl_headers) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curl_headers);
        }
    }

    /* Set body if present (variant: some/none) */
    if (body_val && body_val->kind == EAST_VAL_VARIANT) {
        if (strcmp(body_val->data.variant.case_name, "some") == 0 &&
            body_val->data.variant.value != NULL) {
            const char *body_data = body_val->data.variant.value->data.string.data;
            size_t body_len = body_val->data.variant.value->data.string.len;
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_data);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
        }
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, east_curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wb);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, east_curl_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, resp_headers);

    CURLcode res = curl_easy_perform(curl);

    long status_code = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
    }

    if (curl_headers) {
        curl_slist_free_all(curl_headers);
    }
    curl_easy_cleanup(curl);

    /* Build response struct */
    const char *field_names[] = { "status", "statusText", "headers", "body", "ok" };
    EastValue *field_values[5];
    field_values[0] = east_integer((int64_t)status_code);
    field_values[1] = east_string(res == CURLE_OK ? "OK" : curl_easy_strerror(res));
    field_values[2] = resp_headers;
    field_values[3] = (wb.data && res == CURLE_OK)
                          ? east_string_len(wb.data, wb.len)
                          : east_string("");
    field_values[4] = east_boolean(status_code >= 200 && status_code < 300);

    write_buffer_free(&wb);
    return eval_ok(east_struct_new(field_names, field_values, 5, NULL));
}

#else /* !EAST_HAS_CURL */

/* ========================================================================
 * Stub implementations when cURL is not available
 * ======================================================================== */

static EvalResult fetch_get(EastValue **args, size_t num_args) {
    (void)args;
    (void)num_args;
    return eval_ok(east_string(""));
}

static EvalResult fetch_get_bytes(EastValue **args, size_t num_args) {
    (void)args;
    (void)num_args;
    return eval_ok(east_blob(NULL, 0));
}

static EvalResult fetch_post(EastValue **args, size_t num_args) {
    (void)args;
    (void)num_args;
    return eval_ok(east_string(""));
}

static EvalResult fetch_request(EastValue **args, size_t num_args) {
    (void)args;
    (void)num_args;

    const char *field_names[] = { "status", "statusText", "headers", "body", "ok" };
    EastValue *field_values[5];
    field_values[0] = east_integer(0);
    field_values[1] = east_string("curl not available");
    field_values[2] = east_dict_new(&east_string_type, &east_string_type);
    field_values[3] = east_string("");
    field_values[4] = east_boolean(false);
    return eval_ok(east_struct_new(field_names, field_values, 5, NULL));
}

#endif /* EAST_HAS_CURL */

void east_std_register_fetch(PlatformRegistry *reg) {
    platform_registry_add(reg, "fetch_get", fetch_get, false);
    platform_registry_add(reg, "fetch_get_bytes", fetch_get_bytes, false);
    platform_registry_add(reg, "fetch_post", fetch_post, false);
    platform_registry_add(reg, "fetch_request", fetch_request, false);
}
