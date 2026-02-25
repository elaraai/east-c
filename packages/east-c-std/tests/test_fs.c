/*
 * Tests for east-c-std filesystem platform functions.
 *
 * Covers: writing a temp file, reading it back, checking existence,
 *         deleting, and verifying deletion.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <east/types.h>
#include <east/values.h>
#include <east/eval_result.h>
#include <east/platform.h>
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

/* Shared registry. */
static PlatformRegistry *reg;

/* Temp file path used across tests. */
static const char *TEMP_PATH = "/tmp/east_c_test_fs_XXXXXX.txt";
static char temp_file[256];

static void make_temp_path(void) {
    snprintf(temp_file, sizeof(temp_file), "/tmp/east_c_test_fs_%d.txt", (int)getpid());
}

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

TEST(register_fs) {
    PlatformFn write_fn = platform_registry_get(reg, "fs_write_file", NULL, 0);
    ASSERT(write_fn != NULL);

    PlatformFn read_fn = platform_registry_get(reg, "fs_read_file", NULL, 0);
    ASSERT(read_fn != NULL);

    PlatformFn exists_fn = platform_registry_get(reg, "fs_exists", NULL, 0);
    ASSERT(exists_fn != NULL);

    PlatformFn delete_fn = platform_registry_get(reg, "fs_delete_file", NULL, 0);
    ASSERT(delete_fn != NULL);
}

TEST(write_and_read_file) {
    PlatformFn write_fn = platform_registry_get(reg, "fs_write_file", NULL, 0);
    PlatformFn read_fn = platform_registry_get(reg, "fs_read_file", NULL, 0);
    ASSERT(write_fn != NULL);
    ASSERT(read_fn != NULL);

    /* Write content. */
    EastValue *path_val = east_string(temp_file);
    EastValue *content = east_string("hello east-c");
    EastValue *write_args[] = {path_val, content};
    EvalResult wr = write_fn(write_args, 2);
    ASSERT(wr.value != NULL);

    /* Read it back. */
    EastValue *read_args[] = {path_val};
    EvalResult read_result = read_fn(read_args, 1);
    ASSERT(read_result.value != NULL);
    ASSERT_EQ_INT(read_result.value->kind, EAST_VAL_STRING);
    ASSERT_EQ_STR(read_result.value->data.string.data, "hello east-c");

    east_value_release(path_val);
    east_value_release(content);
    east_value_release(read_result.value);
}

TEST(file_exists) {
    PlatformFn exists_fn = platform_registry_get(reg, "fs_exists", NULL, 0);
    ASSERT(exists_fn != NULL);

    /* The temp file should exist from the previous test. */
    EastValue *path_val = east_string(temp_file);
    EastValue *args[] = {path_val};
    EvalResult result = exists_fn(args, 1);
    ASSERT(result.value != NULL);
    ASSERT_EQ_INT(result.value->kind, EAST_VAL_BOOLEAN);
    ASSERT(result.value->data.boolean == true);

    east_value_release(path_val);
    east_value_release(result.value);
}

TEST(file_is_file) {
    PlatformFn is_file_fn = platform_registry_get(reg, "fs_is_file", NULL, 0);
    ASSERT(is_file_fn != NULL);

    EastValue *path_val = east_string(temp_file);
    EastValue *args[] = {path_val};
    EvalResult result = is_file_fn(args, 1);
    ASSERT(result.value != NULL);
    ASSERT_EQ_INT(result.value->kind, EAST_VAL_BOOLEAN);
    ASSERT(result.value->data.boolean == true);

    east_value_release(path_val);
    east_value_release(result.value);
}

TEST(delete_file) {
    PlatformFn delete_fn = platform_registry_get(reg, "fs_delete_file", NULL, 0);
    PlatformFn exists_fn = platform_registry_get(reg, "fs_exists", NULL, 0);
    ASSERT(delete_fn != NULL);
    ASSERT(exists_fn != NULL);

    /* Delete the temp file. */
    EastValue *path_val = east_string(temp_file);
    EastValue *del_args[] = {path_val};
    EvalResult dr = delete_fn(del_args, 1);
    ASSERT(dr.value != NULL);

    /* Verify it no longer exists. */
    EastValue *exist_args[] = {path_val};
    EvalResult result = exists_fn(exist_args, 1);
    ASSERT(result.value != NULL);
    ASSERT_EQ_INT(result.value->kind, EAST_VAL_BOOLEAN);
    ASSERT(result.value->data.boolean == false);

    east_value_release(path_val);
    east_value_release(result.value);
}

TEST(read_nonexistent_file) {
    PlatformFn read_fn = platform_registry_get(reg, "fs_read_file", NULL, 0);
    ASSERT(read_fn != NULL);

    EastValue *path_val = east_string("/tmp/east_c_test_nonexistent_file_xyz.txt");
    EastValue *args[] = {path_val};
    EvalResult result = read_fn(args, 1);
    ASSERT(result.value != NULL);
    /* Should return empty string for nonexistent file. */
    ASSERT_EQ_INT(result.value->kind, EAST_VAL_STRING);
    ASSERT_EQ_INT((int64_t)result.value->data.string.len, 0);

    east_value_release(path_val);
    east_value_release(result.value);
}

TEST(append_file) {
    PlatformFn write_fn = platform_registry_get(reg, "fs_write_file", NULL, 0);
    PlatformFn append_fn = platform_registry_get(reg, "fs_append_file", NULL, 0);
    PlatformFn read_fn = platform_registry_get(reg, "fs_read_file", NULL, 0);
    PlatformFn delete_fn = platform_registry_get(reg, "fs_delete_file", NULL, 0);
    ASSERT(write_fn != NULL);
    ASSERT(append_fn != NULL);
    ASSERT(read_fn != NULL);

    char append_file_path[256];
    snprintf(append_file_path, sizeof(append_file_path),
             "/tmp/east_c_test_append_%d.txt", (int)getpid());

    EastValue *path_val = east_string(append_file_path);
    EastValue *content1 = east_string("hello");
    EastValue *content2 = east_string(" world");

    /* Write initial content. */
    EastValue *write_args[] = {path_val, content1};
    write_fn(write_args, 2);

    /* Append more content. */
    EastValue *append_args[] = {path_val, content2};
    append_fn(append_args, 2);

    /* Read back. */
    EastValue *read_args[] = {path_val};
    EvalResult result = read_fn(read_args, 1);
    ASSERT(result.value != NULL);
    ASSERT_EQ_STR(result.value->data.string.data, "hello world");

    /* Cleanup. */
    EastValue *del_args[] = {path_val};
    delete_fn(del_args, 1);

    east_value_release(path_val);
    east_value_release(content1);
    east_value_release(content2);
    east_value_release(result.value);
}

TEST(exists_nonexistent) {
    PlatformFn exists_fn = platform_registry_get(reg, "fs_exists", NULL, 0);
    ASSERT(exists_fn != NULL);

    EastValue *path_val = east_string("/tmp/east_c_test_no_such_file_99999.txt");
    EastValue *args[] = {path_val};
    EvalResult result = exists_fn(args, 1);
    ASSERT(result.value != NULL);
    ASSERT(result.value->data.boolean == false);

    east_value_release(path_val);
    east_value_release(result.value);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("test_fs:\n");

    reg = platform_registry_new();
    east_std_register_fs(reg);
    make_temp_path();

    /* Clean up any leftover temp file from a previous run. */
    unlink(temp_file);

    RUN_TEST(register_fs);
    RUN_TEST(write_and_read_file);
    RUN_TEST(file_exists);
    RUN_TEST(file_is_file);
    RUN_TEST(delete_file);
    RUN_TEST(read_nonexistent_file);
    RUN_TEST(append_file);
    RUN_TEST(exists_nonexistent);

    platform_registry_free(reg);

    printf("\n  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
