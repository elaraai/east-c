/*
 * Path manipulation platform functions for East.
 *
 * Provides path operations for East programs running in C.
 */

#include "east_std/east_std.h"
#include <east/values.h>
#include <east/eval_result.h>
#include <east/types.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

static EvalResult path_join(EastValue **args, size_t num_args) {
    (void)num_args;
    EastValue *segments = args[0];
    size_t count = east_array_len(segments);

    if (count == 0) {
        return eval_ok(east_string("."));
    }

    /* Calculate total length needed */
    size_t total_len = 0;
    for (size_t i = 0; i < count; i++) {
        EastValue *seg = east_array_get(segments, i);
        total_len += seg->data.string.len;
        if (i < count - 1) {
            total_len += 1; /* for '/' separator */
        }
    }

    char *buf = malloc(total_len + 1);
    if (!buf) {
        return eval_ok(east_string("."));
    }

    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        EastValue *seg = east_array_get(segments, i);
        size_t seg_len = seg->data.string.len;
        memcpy(buf + pos, seg->data.string.data, seg_len);
        pos += seg_len;
        if (i < count - 1) {
            buf[pos] = '/';
            pos++;
        }
    }
    buf[pos] = '\0';

    EastValue *result = east_string_len(buf, pos);
    free(buf);
    return eval_ok(result);
}

static EvalResult path_resolve(EastValue **args, size_t num_args) {
    (void)num_args;
    const char *path = args[0]->data.string.data;

    /* If already absolute, return as-is */
    if (path[0] == '/') {
        return eval_ok(east_string(path));
    }

    /* Prepend cwd for relative paths (like Node.js path.resolve) */
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        size_t cwd_len = strlen(cwd);
        size_t path_len = args[0]->data.string.len;
        size_t total = cwd_len + 1 + path_len;
        char *buf = malloc(total + 1);
        if (buf) {
            memcpy(buf, cwd, cwd_len);
            buf[cwd_len] = '/';
            memcpy(buf + cwd_len + 1, path, path_len);
            buf[total] = '\0';
            EastValue *result = east_string_len(buf, total);
            free(buf);
            return eval_ok(result);
        }
    }
    return eval_ok(east_string(path));
}

static EvalResult path_dirname(EastValue **args, size_t num_args) {
    (void)num_args;
    const char *path = args[0]->data.string.data;
    size_t len = args[0]->data.string.len;

    /* Find last '/' */
    const char *last_slash = NULL;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/') {
            last_slash = &path[i];
        }
    }

    if (!last_slash) {
        /* No slash found, return empty string (like Python os.path.dirname) */
        return eval_ok(east_string(""));
    }

    size_t dir_len = (size_t)(last_slash - path);
    if (dir_len == 0) {
        /* Root directory */
        return eval_ok(east_string("/"));
    }
    return eval_ok(east_string_len(path, dir_len));
}

static EvalResult path_basename(EastValue **args, size_t num_args) {
    (void)num_args;
    const char *path = args[0]->data.string.data;
    size_t len = args[0]->data.string.len;

    /* Find last '/' */
    const char *last_slash = NULL;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/') {
            last_slash = &path[i];
        }
    }

    if (!last_slash) {
        /* No slash, entire path is basename */
        return eval_ok(east_string(path));
    }

    return eval_ok(east_string(last_slash + 1));
}

static EvalResult path_extname(EastValue **args, size_t num_args) {
    (void)num_args;
    const char *path = args[0]->data.string.data;
    size_t len = args[0]->data.string.len;

    /* Find the basename first (after last /) */
    const char *base = path;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/') {
            base = &path[i + 1];
        }
    }

    /* Find last '.' in basename */
    const char *last_dot = NULL;
    for (const char *p = base; *p; p++) {
        if (*p == '.') {
            last_dot = p;
        }
    }

    if (!last_dot || last_dot == base) {
        /* No dot, or dot is the first char (hidden file) */
        return eval_ok(east_string(""));
    }

    return eval_ok(east_string(last_dot));
}

void east_std_register_path(PlatformRegistry *reg) {
    platform_registry_add(reg, "path_join", path_join, false);
    platform_registry_add(reg, "path_resolve", path_resolve, false);
    platform_registry_add(reg, "path_dirname", path_dirname, false);
    platform_registry_add(reg, "path_basename", path_basename, false);
    platform_registry_add(reg, "path_extname", path_extname, false);
}
