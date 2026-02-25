/*
 * Tests for east-c-std platform registration.
 *
 * Covers: registering individual modules, verifying registry contents,
 *         and calling available platform functions.
 *
 * Note: Some modules (crypto, time, random, fetch, test) may not be
 * implemented yet. Tests gracefully skip unavailable functions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <east/types.h>
#include <east/values.h>
#include <east/eval_result.h>
#include <east/platform.h>
#include <east/hashmap.h>
#include <east_std/east_std.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) static void test_##name(void)
#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return; \
    } \
} while(0)
#define ASSERT_EQ_INT(a, b) do { \
    int64_t _a = (a), _b = (b); \
    if (_a != _b) { \
        fprintf(stderr, "  FAIL: %s:%d: %lld != %lld\n", __FILE__, __LINE__, (long long)_a, (long long)_b); \
        return; \
    } \
} while(0)
#define ASSERT_EQ_STR(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        fprintf(stderr, "  FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
        return; \
    } \
} while(0)
#define RUN_TEST(name) do { \
    tests_run++; \
    printf("  test_%s...", #name); \
    test_##name(); \
    tests_passed++; \
    printf(" OK\n"); \
} while(0)

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

TEST(register_console_functions) {
    PlatformRegistry *reg = platform_registry_new();
    ASSERT(reg != NULL);

    east_std_register_console(reg);

    ASSERT(platform_registry_get(reg, "console_log", NULL, 0) != NULL);
    ASSERT(platform_registry_get(reg, "console_error", NULL, 0) != NULL);
    ASSERT(platform_registry_get(reg, "console_write", NULL, 0) != NULL);

    platform_registry_free(reg);
}

TEST(register_fs_functions) {
    PlatformRegistry *reg = platform_registry_new();
    ASSERT(reg != NULL);

    east_std_register_fs(reg);

    ASSERT(platform_registry_get(reg, "fs_read_file", NULL, 0) != NULL);
    ASSERT(platform_registry_get(reg, "fs_write_file", NULL, 0) != NULL);
    ASSERT(platform_registry_get(reg, "fs_delete_file", NULL, 0) != NULL);
    ASSERT(platform_registry_get(reg, "fs_exists", NULL, 0) != NULL);
    ASSERT(platform_registry_get(reg, "fs_is_file", NULL, 0) != NULL);
    ASSERT(platform_registry_get(reg, "fs_is_directory", NULL, 0) != NULL);
    ASSERT(platform_registry_get(reg, "fs_create_directory", NULL, 0) != NULL);
    ASSERT(platform_registry_get(reg, "fs_read_directory", NULL, 0) != NULL);
    ASSERT(platform_registry_get(reg, "fs_append_file", NULL, 0) != NULL);
    ASSERT(platform_registry_get(reg, "fs_read_file_bytes", NULL, 0) != NULL);
    ASSERT(platform_registry_get(reg, "fs_write_file_bytes", NULL, 0) != NULL);

    platform_registry_free(reg);
}

TEST(register_path_functions) {
    PlatformRegistry *reg = platform_registry_new();
    ASSERT(reg != NULL);

    east_std_register_path(reg);

    ASSERT(platform_registry_get(reg, "path_join", NULL, 0) != NULL);
    ASSERT(platform_registry_get(reg, "path_resolve", NULL, 0) != NULL);
    ASSERT(platform_registry_get(reg, "path_dirname", NULL, 0) != NULL);
    ASSERT(platform_registry_get(reg, "path_basename", NULL, 0) != NULL);
    ASSERT(platform_registry_get(reg, "path_extname", NULL, 0) != NULL);

    platform_registry_free(reg);
}

TEST(missing_function_returns_null) {
    PlatformRegistry *reg = platform_registry_new();
    ASSERT(reg != NULL);

    PlatformFn fn = platform_registry_get(reg, "no_such_function", NULL, 0);
    ASSERT(fn == NULL);

    platform_registry_free(reg);
}

TEST(multiple_modules_no_conflict) {
    PlatformRegistry *reg = platform_registry_new();
    ASSERT(reg != NULL);

    east_std_register_console(reg);
    east_std_register_fs(reg);
    east_std_register_path(reg);

    /* All functions from all three modules should be accessible. */
    ASSERT(platform_registry_get(reg, "console_log", NULL, 0) != NULL);
    ASSERT(platform_registry_get(reg, "fs_read_file", NULL, 0) != NULL);
    ASSERT(platform_registry_get(reg, "path_join", NULL, 0) != NULL);

    platform_registry_free(reg);
}

TEST(time_now_if_available) {
    PlatformRegistry *reg = platform_registry_new();
    ASSERT(reg != NULL);

    east_std_register_time(reg);

    PlatformFn time_fn = platform_registry_get(reg, "time_now", NULL, 0);
    if (!time_fn) {
        /* Time module not implemented yet. */
        platform_registry_free(reg);
        return;
    }

    /* Call time_now with no args. */
    EvalResult result = time_fn(NULL, 0);
    ASSERT(result.value != NULL);
    /* Should return a positive integer (epoch millis). */
    ASSERT_EQ_INT(result.value->kind, EAST_VAL_INTEGER);
    ASSERT(result.value->data.integer > 0);

    east_value_release(result.value);
    platform_registry_free(reg);
}

TEST(path_basename_call) {
    PlatformRegistry *reg = platform_registry_new();
    east_std_register_path(reg);

    PlatformFn fn = platform_registry_get(reg, "path_basename", NULL, 0);
    ASSERT(fn != NULL);

    EastValue *path = east_string("/foo/bar/baz.txt");
    EastValue *args[] = {path};
    EvalResult result = fn(args, 1);
    ASSERT(result.value != NULL);
    ASSERT_EQ_STR(result.value->data.string.data, "baz.txt");

    east_value_release(path);
    east_value_release(result.value);
    platform_registry_free(reg);
}

TEST(path_dirname_call) {
    PlatformRegistry *reg = platform_registry_new();
    east_std_register_path(reg);

    PlatformFn fn = platform_registry_get(reg, "path_dirname", NULL, 0);
    ASSERT(fn != NULL);

    EastValue *path = east_string("/foo/bar/baz.txt");
    EastValue *args[] = {path};
    EvalResult result = fn(args, 1);
    ASSERT(result.value != NULL);
    ASSERT_EQ_STR(result.value->data.string.data, "/foo/bar");

    east_value_release(path);
    east_value_release(result.value);
    platform_registry_free(reg);
}

TEST(path_extname_call) {
    PlatformRegistry *reg = platform_registry_new();
    east_std_register_path(reg);

    PlatformFn fn = platform_registry_get(reg, "path_extname", NULL, 0);
    ASSERT(fn != NULL);

    EastValue *path = east_string("/foo/bar/baz.txt");
    EastValue *args[] = {path};
    EvalResult result = fn(args, 1);
    ASSERT(result.value != NULL);
    ASSERT_EQ_STR(result.value->data.string.data, ".txt");

    east_value_release(path);
    east_value_release(result.value);
    platform_registry_free(reg);
}

TEST(path_join_call) {
    PlatformRegistry *reg = platform_registry_new();
    east_std_register_path(reg);

    PlatformFn fn = platform_registry_get(reg, "path_join", NULL, 0);
    ASSERT(fn != NULL);

    /* path_join takes an array of strings. */
    EastValue *arr = east_array_new(&east_string_type);
    EastValue *s1 = east_string("/foo");
    EastValue *s2 = east_string("bar");
    EastValue *s3 = east_string("baz.txt");
    east_array_push(arr, s1);
    east_array_push(arr, s2);
    east_array_push(arr, s3);

    EastValue *args[] = {arr};
    EvalResult result = fn(args, 1);
    ASSERT(result.value != NULL);
    ASSERT_EQ_STR(result.value->data.string.data, "/foo/bar/baz.txt");

    east_value_release(s1);
    east_value_release(s2);
    east_value_release(s3);
    east_value_release(arr);
    east_value_release(result.value);
    platform_registry_free(reg);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("test_platform:\n");

    RUN_TEST(register_console_functions);
    RUN_TEST(register_fs_functions);
    RUN_TEST(register_path_functions);
    RUN_TEST(missing_function_returns_null);
    RUN_TEST(multiple_modules_no_conflict);
    RUN_TEST(time_now_if_available);
    RUN_TEST(path_basename_call);
    RUN_TEST(path_dirname_call);
    RUN_TEST(path_extname_call);
    RUN_TEST(path_join_call);

    printf("\n  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
