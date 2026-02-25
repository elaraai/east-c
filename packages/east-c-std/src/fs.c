/*
 * Filesystem platform functions for East.
 *
 * Provides filesystem operations for East programs running in C.
 * Uses POSIX APIs for directory and file operations.
 */

#include "east_std/east_std.h"
#include <east/values.h>
#include <east/types.h>
#include <east/eval_result.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

static EvalResult fs_read_file(EastValue **args, size_t num_args) {
    (void)num_args;
    const char *path = args[0]->data.string.data;

    FILE *f = fopen(path, "rb");
    if (!f) {
        return eval_ok(east_string(""));
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fclose(f);
        return eval_ok(east_string(""));
    }

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return eval_ok(east_string(""));
    }

    size_t read_bytes = fread(buf, 1, (size_t)size, f);
    fclose(f);

    buf[read_bytes] = '\0';
    EastValue *result = east_string_len(buf, read_bytes);
    free(buf);
    return eval_ok(result);
}

static EvalResult fs_write_file(EastValue **args, size_t num_args) {
    (void)num_args;
    const char *path = args[0]->data.string.data;
    const char *content = args[1]->data.string.data;
    size_t len = args[1]->data.string.len;

    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(content, 1, len, f);
        fclose(f);
    }
    return eval_ok(east_null());
}

static EvalResult fs_append_file(EastValue **args, size_t num_args) {
    (void)num_args;
    const char *path = args[0]->data.string.data;
    const char *content = args[1]->data.string.data;
    size_t len = args[1]->data.string.len;

    FILE *f = fopen(path, "a");
    if (f) {
        fwrite(content, 1, len, f);
        fclose(f);
    }
    return eval_ok(east_null());
}

static EvalResult fs_delete_file(EastValue **args, size_t num_args) {
    (void)num_args;
    const char *path = args[0]->data.string.data;
    unlink(path);
    return eval_ok(east_null());
}

static EvalResult fs_exists(EastValue **args, size_t num_args) {
    (void)num_args;
    const char *path = args[0]->data.string.data;
    struct stat st;
    return eval_ok(east_boolean(stat(path, &st) == 0));
}

static EvalResult fs_is_file(EastValue **args, size_t num_args) {
    (void)num_args;
    const char *path = args[0]->data.string.data;
    struct stat st;
    if (stat(path, &st) != 0) {
        return eval_ok(east_boolean(false));
    }
    return eval_ok(east_boolean(S_ISREG(st.st_mode)));
}

static EvalResult fs_is_directory(EastValue **args, size_t num_args) {
    (void)num_args;
    const char *path = args[0]->data.string.data;
    struct stat st;
    if (stat(path, &st) != 0) {
        return eval_ok(east_boolean(false));
    }
    return eval_ok(east_boolean(S_ISDIR(st.st_mode)));
}

static EvalResult fs_create_directory(EastValue **args, size_t num_args) {
    (void)num_args;
    const char *path = args[0]->data.string.data;

    /* Create directory with parents, similar to mkdir -p */
    char *tmp = strdup(path);
    if (!tmp) return eval_ok(east_null());

    size_t len = strlen(tmp);
    /* Remove trailing slash */
    if (len > 1 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
    free(tmp);
    return eval_ok(east_null());
}

static EvalResult fs_read_directory(EastValue **args, size_t num_args) {
    (void)num_args;
    const char *path = args[0]->data.string.data;

    EastValue *arr = east_array_new(&east_string_type);

    DIR *dir = opendir(path);
    if (!dir) {
        return eval_ok(arr);
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        EastValue *name = east_string(entry->d_name);
        east_array_push(arr, name);
    }
    closedir(dir);
    return eval_ok(arr);
}

static EvalResult fs_read_file_bytes(EastValue **args, size_t num_args) {
    (void)num_args;
    const char *path = args[0]->data.string.data;

    FILE *f = fopen(path, "rb");
    if (!f) {
        return eval_ok(east_blob(NULL, 0));
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fclose(f);
        return eval_ok(east_blob(NULL, 0));
    }

    uint8_t *buf = malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return eval_ok(east_blob(NULL, 0));
    }

    size_t read_bytes = fread(buf, 1, (size_t)size, f);
    fclose(f);

    EastValue *result = east_blob(buf, read_bytes);
    free(buf);
    return eval_ok(result);
}

static EvalResult fs_write_file_bytes(EastValue **args, size_t num_args) {
    (void)num_args;
    const char *path = args[0]->data.string.data;
    const uint8_t *data = args[1]->data.blob.data;
    size_t len = args[1]->data.blob.len;

    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, len, f);
        fclose(f);
    }
    return eval_ok(east_null());
}

void east_std_register_fs(PlatformRegistry *reg) {
    platform_registry_add(reg, "fs_read_file", fs_read_file, false);
    platform_registry_add(reg, "fs_write_file", fs_write_file, false);
    platform_registry_add(reg, "fs_append_file", fs_append_file, false);
    platform_registry_add(reg, "fs_delete_file", fs_delete_file, false);
    platform_registry_add(reg, "fs_exists", fs_exists, false);
    platform_registry_add(reg, "fs_is_file", fs_is_file, false);
    platform_registry_add(reg, "fs_is_directory", fs_is_directory, false);
    platform_registry_add(reg, "fs_create_directory", fs_create_directory, false);
    platform_registry_add(reg, "fs_read_directory", fs_read_directory, false);
    platform_registry_add(reg, "fs_read_file_bytes", fs_read_file_bytes, false);
    platform_registry_add(reg, "fs_write_file_bytes", fs_write_file_bytes, false);
}
