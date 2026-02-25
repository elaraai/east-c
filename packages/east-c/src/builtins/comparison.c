/*
 * Comparison builtin functions.
 *
 * These are factory builtins -- they take a type parameter to specialize
 * the comparison. The C runtime uses east_value_equal / east_value_compare
 * which already operate on the generic EastValue representation.
 */
#include "east/builtins.h"
#include "east/values.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* --- static implementations --- */

static EastValue *comparison_is(EastValue **args, size_t n) {
    (void)n;
    EastValue *a = args[0], *b = args[1];
    /* Pointer identity is always true */
    if (a == b) return east_boolean(true);
    if (!a || !b) return east_boolean(false);
    if (a->kind != b->kind) return east_boolean(false);

    /* Mutable types: identity comparison (different pointers = not identical) */
    switch (a->kind) {
    case EAST_VAL_ARRAY:
    case EAST_VAL_SET:
    case EAST_VAL_DICT:
    case EAST_VAL_REF:
    case EAST_VAL_VECTOR:
    case EAST_VAL_MATRIX:
        return east_boolean(false);
    default:
        break;
    }

    /* Immutable types: value comparison (isFor semantics) */
    switch (a->kind) {
    case EAST_VAL_NULL:
        return east_boolean(true);
    case EAST_VAL_BOOLEAN:
        return east_boolean(a->data.boolean == b->data.boolean);
    case EAST_VAL_INTEGER:
        return east_boolean(a->data.integer == b->data.integer);
    case EAST_VAL_FLOAT:
        /* isFor Float: NaN is NaN, +0 === -0 (JS === semantics) */
        if (isnan(a->data.float64))
            return east_boolean(isnan(b->data.float64) ? true : false);
        return east_boolean(a->data.float64 == b->data.float64);
    case EAST_VAL_STRING:
        if (a->data.string.len != b->data.string.len)
            return east_boolean(false);
        return east_boolean(
            memcmp(a->data.string.data, b->data.string.data,
                   a->data.string.len) == 0);
    case EAST_VAL_DATETIME:
        return east_boolean(a->data.datetime == b->data.datetime);
    case EAST_VAL_BLOB:
        if (a->data.blob.len != b->data.blob.len)
            return east_boolean(false);
        if (a->data.blob.len == 0) return east_boolean(true);
        return east_boolean(
            memcmp(a->data.blob.data, b->data.blob.data,
                   a->data.blob.len) == 0);
    case EAST_VAL_STRUCT:
    case EAST_VAL_VARIANT:
        /* Struct/Variant are immutable - use deep isFor comparison
         * which is the same as east_value_equal for these types,
         * except Float uses === not Object.is. For simplicity,
         * use east_value_equal which is close enough. */
        return east_boolean(east_value_equal(a, b));
    case EAST_VAL_FUNCTION:
        return east_boolean(a->data.function.compiled ==
                            b->data.function.compiled);
    default:
        return east_boolean(false);
    }
}

static EastValue *comparison_equal(EastValue **args, size_t n) {
    (void)n;
    return east_boolean(east_value_equal(args[0], args[1]));
}

static EastValue *comparison_not_equal(EastValue **args, size_t n) {
    (void)n;
    return east_boolean(!east_value_equal(args[0], args[1]));
}

static EastValue *comparison_less(EastValue **args, size_t n) {
    (void)n;
    return east_boolean(east_value_compare(args[0], args[1]) < 0);
}

static EastValue *comparison_less_equal(EastValue **args, size_t n) {
    (void)n;
    return east_boolean(east_value_compare(args[0], args[1]) <= 0);
}

static EastValue *comparison_greater(EastValue **args, size_t n) {
    (void)n;
    return east_boolean(east_value_compare(args[0], args[1]) > 0);
}

static EastValue *comparison_greater_equal(EastValue **args, size_t n) {
    (void)n;
    return east_boolean(east_value_compare(args[0], args[1]) >= 0);
}

/* --- factory functions (type param selects the comparator, but our C
       implementation is generic over EastValue) --- */

static BuiltinImpl is_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return comparison_is; }
static BuiltinImpl equal_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return comparison_equal; }
static BuiltinImpl not_equal_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return comparison_not_equal; }
static BuiltinImpl less_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return comparison_less; }
static BuiltinImpl less_equal_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return comparison_less_equal; }
static BuiltinImpl greater_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return comparison_greater; }
static BuiltinImpl greater_equal_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return comparison_greater_equal; }

/* --- registration --- */

void east_register_comparison_builtins(BuiltinRegistry *reg) {
    builtin_registry_register(reg, "Is", is_factory);
    builtin_registry_register(reg, "Equal", equal_factory);
    builtin_registry_register(reg, "NotEqual", not_equal_factory);
    builtin_registry_register(reg, "Less", less_factory);
    builtin_registry_register(reg, "LessEqual", less_equal_factory);
    builtin_registry_register(reg, "Greater", greater_factory);
    builtin_registry_register(reg, "GreaterEqual", greater_equal_factory);
}
