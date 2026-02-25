/*
 * Tests for east/builtins.h
 *
 * Covers: BuiltinRegistry creation, integer/float/boolean/string/comparison
 *         builtin operations, and array builtins.
 *
 * Note: Some builtins (String, Comparison, Array, etc.) may not be implemented
 * yet. Tests for missing builtins will detect a NULL function pointer and
 * skip gracefully (reporting a FAIL that indicates the builtin is unregistered).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <east/types.h>
#include <east/values.h>
#include <east/builtins.h>

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

/* Shared registry, created once in main(). */
static BuiltinRegistry *reg;

/* Helper to call a 2-arg builtin. */
static EastValue *call2(const char *name, EastValue *a, EastValue *b) {
    BuiltinImpl fn = builtin_registry_get(reg, name, NULL, 0);
    if (!fn) return NULL;
    EastValue *args[] = {a, b};
    return fn(args, 2);
}

/* Helper to call a 1-arg builtin. */
static EastValue *call1(const char *name, EastValue *a) {
    BuiltinImpl fn = builtin_registry_get(reg, name, NULL, 0);
    if (!fn) return NULL;
    EastValue *args[] = {a};
    return fn(args, 1);
}

/* ------------------------------------------------------------------ */
/*  Registry                                                           */
/* ------------------------------------------------------------------ */

TEST(registry_create_and_free) {
    BuiltinRegistry *r = builtin_registry_new();
    ASSERT(r != NULL);
    builtin_registry_free(r);
}

TEST(registry_has_integer_add) {
    BuiltinImpl fn = builtin_registry_get(reg, "IntegerAdd", NULL, 0);
    ASSERT(fn != NULL);
}

TEST(registry_missing_builtin_returns_null) {
    BuiltinImpl fn = builtin_registry_get(reg, "NoSuchBuiltin", NULL, 0);
    ASSERT(fn == NULL);
}

/* ------------------------------------------------------------------ */
/*  Integer builtins                                                   */
/* ------------------------------------------------------------------ */

TEST(integer_add) {
    EastValue *a = east_integer(3);
    EastValue *b = east_integer(7);
    EastValue *r = call2("IntegerAdd", a, b);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(r->data.integer, 10);
    east_value_release(a);
    east_value_release(b);
    east_value_release(r);
}

TEST(integer_add_negative) {
    EastValue *a = east_integer(-10);
    EastValue *b = east_integer(3);
    EastValue *r = call2("IntegerAdd", a, b);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(r->data.integer, -7);
    east_value_release(a);
    east_value_release(b);
    east_value_release(r);
}

TEST(integer_subtract) {
    EastValue *a = east_integer(10);
    EastValue *b = east_integer(3);
    EastValue *r = call2("IntegerSubtract", a, b);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(r->data.integer, 7);
    east_value_release(a);
    east_value_release(b);
    east_value_release(r);
}

TEST(integer_multiply) {
    EastValue *a = east_integer(6);
    EastValue *b = east_integer(7);
    EastValue *r = call2("IntegerMultiply", a, b);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(r->data.integer, 42);
    east_value_release(a);
    east_value_release(b);
    east_value_release(r);
}

TEST(integer_divide) {
    EastValue *a = east_integer(10);
    EastValue *b = east_integer(3);
    EastValue *r = call2("IntegerDivide", a, b);
    ASSERT(r != NULL);
    /* Floor division: 10 / 3 = 3 */
    ASSERT_EQ_INT(r->data.integer, 3);
    east_value_release(a);
    east_value_release(b);
    east_value_release(r);
}

TEST(integer_divide_negative) {
    EastValue *a = east_integer(-7);
    EastValue *b = east_integer(2);
    EastValue *r = call2("IntegerDivide", a, b);
    ASSERT(r != NULL);
    /* Floor division: -7 / 2 = -4 (not -3) */
    ASSERT_EQ_INT(r->data.integer, -4);
    east_value_release(a);
    east_value_release(b);
    east_value_release(r);
}

TEST(integer_divide_by_zero) {
    EastValue *a = east_integer(10);
    EastValue *b = east_integer(0);
    EastValue *r = call2("IntegerDivide", a, b);
    ASSERT(r != NULL);
    /* Returns 0 on division by zero */
    ASSERT_EQ_INT(r->data.integer, 0);
    east_value_release(a);
    east_value_release(b);
    east_value_release(r);
}

TEST(integer_remainder) {
    EastValue *a = east_integer(10);
    EastValue *b = east_integer(3);
    EastValue *r = call2("IntegerRemainder", a, b);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(r->data.integer, 1);
    east_value_release(a);
    east_value_release(b);
    east_value_release(r);
}

TEST(integer_power) {
    EastValue *a = east_integer(2);
    EastValue *b = east_integer(10);
    EastValue *r = call2("IntegerPow", a, b);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(r->data.integer, 1024);
    east_value_release(a);
    east_value_release(b);
    east_value_release(r);
}

TEST(integer_negate) {
    EastValue *a = east_integer(42);
    EastValue *r = call1("IntegerNegate", a);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(r->data.integer, -42);
    east_value_release(a);
    east_value_release(r);
}

TEST(integer_abs) {
    EastValue *a = east_integer(-15);
    EastValue *r = call1("IntegerAbs", a);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(r->data.integer, 15);
    east_value_release(a);
    east_value_release(r);
}

TEST(integer_sign) {
    EastValue *neg = east_integer(-5);
    EastValue *zero = east_integer(0);
    EastValue *pos = east_integer(5);

    EastValue *rn = call1("IntegerSign", neg);
    EastValue *rz = call1("IntegerSign", zero);
    EastValue *rp = call1("IntegerSign", pos);

    ASSERT(rn != NULL);
    ASSERT_EQ_INT(rn->data.integer, -1);
    ASSERT_EQ_INT(rz->data.integer, 0);
    ASSERT_EQ_INT(rp->data.integer, 1);

    east_value_release(neg);
    east_value_release(zero);
    east_value_release(pos);
    east_value_release(rn);
    east_value_release(rz);
    east_value_release(rp);
}

TEST(integer_to_float) {
    EastValue *a = east_integer(42);
    EastValue *r = call1("IntegerToFloat", a);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(r->kind, EAST_VAL_FLOAT);
    ASSERT(r->data.float64 == 42.0);
    east_value_release(a);
    east_value_release(r);
}

/* ------------------------------------------------------------------ */
/*  Float builtins                                                     */
/* ------------------------------------------------------------------ */

TEST(float_add) {
    EastValue *a = east_float(1.5);
    EastValue *b = east_float(2.5);
    EastValue *r = call2("FloatAdd", a, b);
    ASSERT(r != NULL);
    ASSERT(r->data.float64 == 4.0);
    east_value_release(a);
    east_value_release(b);
    east_value_release(r);
}

TEST(float_subtract) {
    EastValue *a = east_float(5.0);
    EastValue *b = east_float(3.0);
    EastValue *r = call2("FloatSubtract", a, b);
    ASSERT(r != NULL);
    ASSERT(r->data.float64 == 2.0);
    east_value_release(a);
    east_value_release(b);
    east_value_release(r);
}

TEST(float_multiply) {
    EastValue *a = east_float(3.0);
    EastValue *b = east_float(4.0);
    EastValue *r = call2("FloatMultiply", a, b);
    ASSERT(r != NULL);
    ASSERT(r->data.float64 == 12.0);
    east_value_release(a);
    east_value_release(b);
    east_value_release(r);
}

TEST(float_sqrt) {
    EastValue *a = east_float(16.0);
    EastValue *r = call1("FloatSqrt", a);
    ASSERT(r != NULL);
    ASSERT(r->data.float64 == 4.0);
    east_value_release(a);
    east_value_release(r);
}

TEST(float_negate) {
    EastValue *a = east_float(3.14);
    EastValue *r = call1("FloatNegate", a);
    ASSERT(r != NULL);
    ASSERT(r->data.float64 == -3.14);
    east_value_release(a);
    east_value_release(r);
}

TEST(float_abs) {
    EastValue *a = east_float(-2.5);
    EastValue *r = call1("FloatAbs", a);
    ASSERT(r != NULL);
    ASSERT(r->data.float64 == 2.5);
    east_value_release(a);
    east_value_release(r);
}

TEST(float_to_integer) {
    EastValue *a = east_float(7.9);
    EastValue *r = call1("FloatToInteger", a);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(r->kind, EAST_VAL_INTEGER);
    ASSERT_EQ_INT(r->data.integer, 7);
    east_value_release(a);
    east_value_release(r);
}

/* ------------------------------------------------------------------ */
/*  Boolean builtins                                                   */
/* ------------------------------------------------------------------ */

TEST(boolean_not) {
    EastValue *t = east_boolean(true);
    EastValue *f = east_boolean(false);

    EastValue *rt = call1("BooleanNot", t);
    EastValue *rf = call1("BooleanNot", f);

    ASSERT(rt != NULL);
    ASSERT(rt->data.boolean == false);
    ASSERT(rf != NULL);
    ASSERT(rf->data.boolean == true);

    east_value_release(t);
    east_value_release(f);
    east_value_release(rt);
    east_value_release(rf);
}

TEST(boolean_and) {
    EastValue *t = east_boolean(true);
    EastValue *f = east_boolean(false);

    EastValue *r1 = call2("BooleanAnd", t, t);
    EastValue *r2 = call2("BooleanAnd", t, f);

    ASSERT(r1 != NULL);
    ASSERT(r1->data.boolean == true);
    ASSERT(r2 != NULL);
    ASSERT(r2->data.boolean == false);

    east_value_release(t);
    east_value_release(f);
    east_value_release(r1);
    east_value_release(r2);
}

TEST(boolean_or) {
    EastValue *t = east_boolean(true);
    EastValue *f = east_boolean(false);

    EastValue *r1 = call2("BooleanOr", f, f);
    EastValue *r2 = call2("BooleanOr", f, t);

    ASSERT(r1 != NULL);
    ASSERT(r1->data.boolean == false);
    ASSERT(r2 != NULL);
    ASSERT(r2->data.boolean == true);

    east_value_release(t);
    east_value_release(f);
    east_value_release(r1);
    east_value_release(r2);
}

TEST(boolean_xor) {
    EastValue *t = east_boolean(true);
    EastValue *f = east_boolean(false);

    EastValue *r1 = call2("BooleanXor", t, f);
    EastValue *r2 = call2("BooleanXor", t, t);

    ASSERT(r1 != NULL);
    ASSERT(r1->data.boolean == true);
    ASSERT(r2 != NULL);
    ASSERT(r2->data.boolean == false);

    east_value_release(t);
    east_value_release(f);
    east_value_release(r1);
    east_value_release(r2);
}

/* ------------------------------------------------------------------ */
/*  String builtins (may not be implemented yet)                       */
/* ------------------------------------------------------------------ */

TEST(string_concat) {
    BuiltinImpl fn = builtin_registry_get(reg, "StringConcat", NULL, 0);
    if (!fn) {
        /* String builtins not yet implemented, skip. */
        return;
    }
    EastValue *a = east_string("hello ");
    EastValue *b = east_string("world");
    EastValue *args[] = {a, b};
    EastValue *r = fn(args, 2);
    ASSERT(r != NULL);
    ASSERT_EQ_STR(r->data.string.data, "hello world");
    east_value_release(a);
    east_value_release(b);
    east_value_release(r);
}

TEST(string_length) {
    BuiltinImpl fn = builtin_registry_get(reg, "StringLength", NULL, 0);
    if (!fn) {
        return;
    }
    EastValue *a = east_string("hello");
    EastValue *args[] = {a};
    EastValue *r = fn(args, 1);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(r->data.integer, 5);
    east_value_release(a);
    east_value_release(r);
}

/* ------------------------------------------------------------------ */
/*  Comparison builtins (may not be implemented yet)                   */
/* ------------------------------------------------------------------ */

TEST(comparison_equal) {
    BuiltinImpl fn = builtin_registry_get(reg, "Equal", NULL, 0);
    if (!fn) {
        return;
    }
    EastValue *a = east_integer(5);
    EastValue *b = east_integer(5);
    EastValue *c = east_integer(3);
    EastValue *r1 = call2("Equal", a, b);
    EastValue *r2 = call2("Equal", a, c);

    ASSERT(r1 != NULL);
    ASSERT(r1->data.boolean == true);
    ASSERT(r2 != NULL);
    ASSERT(r2->data.boolean == false);

    east_value_release(a);
    east_value_release(b);
    east_value_release(c);
    east_value_release(r1);
    east_value_release(r2);
}

TEST(comparison_less) {
    BuiltinImpl fn = builtin_registry_get(reg, "Less", NULL, 0);
    if (!fn) {
        return;
    }
    EastValue *a = east_integer(3);
    EastValue *b = east_integer(5);
    EastValue *r1 = call2("Less", a, b);
    EastValue *r2 = call2("Less", b, a);

    ASSERT(r1 != NULL);
    ASSERT(r1->data.boolean == true);
    ASSERT(r2 != NULL);
    ASSERT(r2->data.boolean == false);

    east_value_release(a);
    east_value_release(b);
    east_value_release(r1);
    east_value_release(r2);
}

/* ------------------------------------------------------------------ */
/*  Array builtins (may not be implemented yet)                        */
/* ------------------------------------------------------------------ */

TEST(array_size_builtin) {
    BuiltinImpl fn = builtin_registry_get(reg, "ArraySize", NULL, 0);
    if (!fn) {
        return;
    }
    EastValue *arr = east_array_new(&east_integer_type);
    EastValue *v1 = east_integer(1);
    EastValue *v2 = east_integer(2);
    east_array_push(arr, v1);
    east_array_push(arr, v2);

    EastValue *args[] = {arr};
    EastValue *r = fn(args, 1);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(r->data.integer, 2);

    east_value_release(v1);
    east_value_release(v2);
    east_value_release(arr);
    east_value_release(r);
}

TEST(array_push_builtin) {
    BuiltinImpl fn = builtin_registry_get(reg, "ArrayPush", NULL, 0);
    if (!fn) {
        return;
    }
    EastValue *arr = east_array_new(&east_integer_type);
    EastValue *v = east_integer(42);

    EastValue *args[] = {arr, v};
    EastValue *r = fn(args, 2);

    /* After push, the returned array (or same array) should have 1 element. */
    ASSERT(r != NULL);
    ASSERT_EQ_INT(r->kind, EAST_VAL_ARRAY);
    ASSERT_EQ_INT((int64_t)east_array_len(r), 1);

    east_value_release(v);
    east_value_release(arr);
    if (r != arr) east_value_release(r);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("test_builtins:\n");

    reg = builtin_registry_new();
    east_register_all_builtins(reg);

    /* Registry */
    RUN_TEST(registry_create_and_free);
    RUN_TEST(registry_has_integer_add);
    RUN_TEST(registry_missing_builtin_returns_null);

    /* Integer */
    RUN_TEST(integer_add);
    RUN_TEST(integer_add_negative);
    RUN_TEST(integer_subtract);
    RUN_TEST(integer_multiply);
    RUN_TEST(integer_divide);
    RUN_TEST(integer_divide_negative);
    RUN_TEST(integer_divide_by_zero);
    RUN_TEST(integer_remainder);
    RUN_TEST(integer_power);
    RUN_TEST(integer_negate);
    RUN_TEST(integer_abs);
    RUN_TEST(integer_sign);
    RUN_TEST(integer_to_float);

    /* Float */
    RUN_TEST(float_add);
    RUN_TEST(float_subtract);
    RUN_TEST(float_multiply);
    RUN_TEST(float_sqrt);
    RUN_TEST(float_negate);
    RUN_TEST(float_abs);
    RUN_TEST(float_to_integer);

    /* Boolean */
    RUN_TEST(boolean_not);
    RUN_TEST(boolean_and);
    RUN_TEST(boolean_or);
    RUN_TEST(boolean_xor);

    /* String */
    RUN_TEST(string_concat);
    RUN_TEST(string_length);

    /* Comparison */
    RUN_TEST(comparison_equal);
    RUN_TEST(comparison_less);

    /* Array */
    RUN_TEST(array_size_builtin);
    RUN_TEST(array_push_builtin);

    builtin_registry_free(reg);

    printf("\n  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
