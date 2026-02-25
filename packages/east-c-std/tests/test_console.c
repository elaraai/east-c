/*
 * Tests for east-c-std console platform functions.
 *
 * Covers: registering console functions and calling console_log.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

TEST(register_console) {
    PlatformRegistry *reg = platform_registry_new();
    ASSERT(reg != NULL);

    east_std_register_console(reg);

    /* Verify the functions were registered. */
    PlatformFn log_fn = platform_registry_get(reg, "console_log", NULL, 0);
    ASSERT(log_fn != NULL);

    PlatformFn error_fn = platform_registry_get(reg, "console_error", NULL, 0);
    ASSERT(error_fn != NULL);

    PlatformFn write_fn = platform_registry_get(reg, "console_write", NULL, 0);
    ASSERT(write_fn != NULL);

    platform_registry_free(reg);
}

TEST(console_log_call) {
    PlatformRegistry *reg = platform_registry_new();
    east_std_register_console(reg);

    PlatformFn log_fn = platform_registry_get(reg, "console_log", NULL, 0);
    ASSERT(log_fn != NULL);

    /* Call console_log with a string. Should print to stdout and return null. */
    EastValue *msg = east_string("test_console: hello from test");
    EastValue *args[] = {msg};

    /* Redirect stdout output -- we just verify no crash. */
    EvalResult result = log_fn(args, 1);
    ASSERT(result.value != NULL);
    ASSERT_EQ_INT(result.value->kind, EAST_VAL_NULL);

    east_value_release(msg);
    platform_registry_free(reg);
}

TEST(console_error_call) {
    PlatformRegistry *reg = platform_registry_new();
    east_std_register_console(reg);

    PlatformFn error_fn = platform_registry_get(reg, "console_error", NULL, 0);
    ASSERT(error_fn != NULL);

    EastValue *msg = east_string("test error message");
    EastValue *args[] = {msg};

    EvalResult result = error_fn(args, 1);
    ASSERT(result.value != NULL);
    ASSERT_EQ_INT(result.value->kind, EAST_VAL_NULL);

    east_value_release(msg);
    platform_registry_free(reg);
}

TEST(console_write_call) {
    PlatformRegistry *reg = platform_registry_new();
    east_std_register_console(reg);

    PlatformFn write_fn = platform_registry_get(reg, "console_write", NULL, 0);
    ASSERT(write_fn != NULL);

    EastValue *msg = east_string("no newline");
    EastValue *args[] = {msg};

    EvalResult result = write_fn(args, 1);
    ASSERT(result.value != NULL);
    ASSERT_EQ_INT(result.value->kind, EAST_VAL_NULL);

    east_value_release(msg);
    platform_registry_free(reg);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("test_console:\n");

    RUN_TEST(register_console);
    RUN_TEST(console_log_call);
    RUN_TEST(console_error_call);
    RUN_TEST(console_write_call);

    printf("\n  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
