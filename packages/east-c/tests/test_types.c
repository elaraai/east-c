/*
 * Tests for east/types.h
 *
 * Covers: type construction, equality, printing, and ref counting.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <east/types.h>

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
/*  Primitive type singletons                                          */
/* ------------------------------------------------------------------ */

TEST(primitive_kinds) {
    ASSERT_EQ_INT(east_null_type.kind, EAST_TYPE_NULL);
    ASSERT_EQ_INT(east_boolean_type.kind, EAST_TYPE_BOOLEAN);
    ASSERT_EQ_INT(east_integer_type.kind, EAST_TYPE_INTEGER);
    ASSERT_EQ_INT(east_float_type.kind, EAST_TYPE_FLOAT);
    ASSERT_EQ_INT(east_string_type.kind, EAST_TYPE_STRING);
    ASSERT_EQ_INT(east_datetime_type.kind, EAST_TYPE_DATETIME);
    ASSERT_EQ_INT(east_blob_type.kind, EAST_TYPE_BLOB);
    ASSERT_EQ_INT(east_never_type.kind, EAST_TYPE_NEVER);
}

TEST(primitive_singletons_have_negative_refcount) {
    /* Singletons must never be freed, indicated by ref_count < 0. */
    ASSERT(east_integer_type.ref_count < 0);
    ASSERT(east_string_type.ref_count < 0);
    ASSERT(east_null_type.ref_count < 0);
}

/* ------------------------------------------------------------------ */
/*  Parameterized types                                                */
/* ------------------------------------------------------------------ */

TEST(array_type) {
    EastType *t = east_array_type(&east_integer_type);
    ASSERT(t != NULL);
    ASSERT_EQ_INT(t->kind, EAST_TYPE_ARRAY);
    ASSERT(t->data.element == &east_integer_type);
    ASSERT_EQ_INT(t->ref_count, 1);
    east_type_release(t);
}

TEST(set_type) {
    EastType *t = east_set_type(&east_string_type);
    ASSERT(t != NULL);
    ASSERT_EQ_INT(t->kind, EAST_TYPE_SET);
    ASSERT(t->data.element == &east_string_type);
    east_type_release(t);
}

TEST(dict_type) {
    EastType *t = east_dict_type(&east_string_type, &east_integer_type);
    ASSERT(t != NULL);
    ASSERT_EQ_INT(t->kind, EAST_TYPE_DICT);
    ASSERT(t->data.dict.key == &east_string_type);
    ASSERT(t->data.dict.value == &east_integer_type);
    east_type_release(t);
}

TEST(struct_type) {
    const char *names[] = {"x", "y"};
    EastType *types[] = {&east_integer_type, &east_float_type};
    EastType *t = east_struct_type(names, types, 2);
    ASSERT(t != NULL);
    ASSERT_EQ_INT(t->kind, EAST_TYPE_STRUCT);
    ASSERT_EQ_INT((int64_t)t->data.struct_.num_fields, 2);
    ASSERT_EQ_STR(t->data.struct_.fields[0].name, "x");
    ASSERT(t->data.struct_.fields[0].type == &east_integer_type);
    ASSERT_EQ_STR(t->data.struct_.fields[1].name, "y");
    ASSERT(t->data.struct_.fields[1].type == &east_float_type);
    east_type_release(t);
}

TEST(variant_type) {
    const char *names[] = {"Some", "None"};
    EastType *types[] = {&east_integer_type, &east_null_type};
    EastType *t = east_variant_type(names, types, 2);
    ASSERT(t != NULL);
    ASSERT_EQ_INT(t->kind, EAST_TYPE_VARIANT);
    ASSERT_EQ_INT((int64_t)t->data.variant.num_cases, 2);
    /* Variant cases are sorted alphabetically, so None < Some */
    ASSERT_EQ_STR(t->data.variant.cases[0].name, "None");
    ASSERT_EQ_STR(t->data.variant.cases[1].name, "Some");
    east_type_release(t);
}

TEST(function_type) {
    EastType *inputs[] = {&east_integer_type, &east_integer_type};
    EastType *t = east_function_type(inputs, 2, &east_integer_type);
    ASSERT(t != NULL);
    ASSERT_EQ_INT(t->kind, EAST_TYPE_FUNCTION);
    ASSERT_EQ_INT((int64_t)t->data.function.num_inputs, 2);
    ASSERT(t->data.function.output == &east_integer_type);
    east_type_release(t);
}

TEST(ref_type) {
    EastType *t = east_ref_type(&east_integer_type);
    ASSERT(t != NULL);
    ASSERT_EQ_INT(t->kind, EAST_TYPE_REF);
    ASSERT(t->data.element == &east_integer_type);
    east_type_release(t);
}

TEST(vector_type) {
    EastType *t = east_vector_type(&east_float_type);
    ASSERT(t != NULL);
    ASSERT_EQ_INT(t->kind, EAST_TYPE_VECTOR);
    ASSERT(t->data.element == &east_float_type);
    east_type_release(t);
}

TEST(matrix_type) {
    EastType *t = east_matrix_type(&east_float_type);
    ASSERT(t != NULL);
    ASSERT_EQ_INT(t->kind, EAST_TYPE_MATRIX);
    ASSERT(t->data.element == &east_float_type);
    east_type_release(t);
}

TEST(recursive_type) {
    /* Create a recursive wrapper and set its inner node */
    EastType *rec = east_recursive_type_new();
    ASSERT(rec != NULL);
    ASSERT_EQ_INT(rec->kind, EAST_TYPE_RECURSIVE);
    ASSERT(rec->data.recursive.node == NULL);

    /* Build an inner type: Array<Recursive-self-ref> */
    EastType *arr = east_array_type(rec);  /* retains rec */
    east_recursive_type_set(rec, arr);
    ASSERT(rec->data.recursive.node == arr);

    /* The inner array's element should point back to the wrapper */
    ASSERT(arr->data.element == rec);

    /* Finalize for cycle-aware cleanup: subtracts internal refs */
    east_recursive_type_finalize(rec);
    ASSERT_EQ_INT(rec->ref_count, 1);  /* only our ref remains */
    ASSERT_EQ_INT(rec->data.recursive.internal_refs, 1);

    /* Releasing rec triggers cycle breaking: frees both rec and arr */
    east_type_release(rec);
}

/* ------------------------------------------------------------------ */
/*  Equality                                                           */
/* ------------------------------------------------------------------ */

TEST(equal_primitives) {
    ASSERT(east_type_equal(&east_integer_type, &east_integer_type));
    ASSERT(east_type_equal(&east_string_type, &east_string_type));
    ASSERT(east_type_equal(&east_null_type, &east_null_type));
}

TEST(not_equal_different_primitives) {
    ASSERT(!east_type_equal(&east_integer_type, &east_string_type));
    ASSERT(!east_type_equal(&east_float_type, &east_boolean_type));
    ASSERT(!east_type_equal(&east_null_type, &east_integer_type));
}

TEST(equal_array_types) {
    EastType *a = east_array_type(&east_integer_type);
    EastType *b = east_array_type(&east_integer_type);
    ASSERT(east_type_equal(a, b));
    east_type_release(a);
    east_type_release(b);
}

TEST(not_equal_array_types_diff_elem) {
    EastType *a = east_array_type(&east_integer_type);
    EastType *b = east_array_type(&east_string_type);
    ASSERT(!east_type_equal(a, b));
    east_type_release(a);
    east_type_release(b);
}

TEST(equal_dict_types) {
    EastType *a = east_dict_type(&east_string_type, &east_integer_type);
    EastType *b = east_dict_type(&east_string_type, &east_integer_type);
    ASSERT(east_type_equal(a, b));
    east_type_release(a);
    east_type_release(b);
}

TEST(not_equal_dict_types_diff_val) {
    EastType *a = east_dict_type(&east_string_type, &east_integer_type);
    EastType *b = east_dict_type(&east_string_type, &east_float_type);
    ASSERT(!east_type_equal(a, b));
    east_type_release(a);
    east_type_release(b);
}

TEST(equal_struct_types) {
    const char *names[] = {"a", "b"};
    EastType *types[] = {&east_integer_type, &east_string_type};
    EastType *a = east_struct_type(names, types, 2);
    EastType *b = east_struct_type(names, types, 2);
    ASSERT(east_type_equal(a, b));
    east_type_release(a);
    east_type_release(b);
}

TEST(equal_recursive_types) {
    /* Same recursive wrapper should be equal to itself (pointer equality) */
    EastType *a = east_recursive_type_new();
    ASSERT(east_type_equal(a, a));
    east_type_release(a);
}

TEST(not_equal_recursive_types_diff_instance) {
    /* Different recursive wrappers are not equal (pointer inequality) */
    EastType *a = east_recursive_type_new();
    EastType *b = east_recursive_type_new();
    ASSERT(!east_type_equal(a, b));
    east_type_release(a);
    east_type_release(b);
}

TEST(equal_null_pointers) {
    ASSERT(!east_type_equal(NULL, &east_integer_type));
    ASSERT(!east_type_equal(&east_integer_type, NULL));
    /* Both NULL should return false per the implementation */
    ASSERT(!east_type_equal(NULL, NULL));
}

/* ------------------------------------------------------------------ */
/*  Printing                                                           */
/* ------------------------------------------------------------------ */

TEST(print_primitives) {
    char buf[128];

    east_type_print(&east_null_type, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "Null");

    east_type_print(&east_boolean_type, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "Boolean");

    east_type_print(&east_integer_type, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "Integer");

    east_type_print(&east_float_type, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "Float");

    east_type_print(&east_string_type, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "String");

    east_type_print(&east_never_type, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "Never");
}

TEST(print_array_type) {
    EastType *t = east_array_type(&east_integer_type);
    char buf[128];
    east_type_print(t, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "Array<Integer>");
    east_type_release(t);
}

TEST(print_set_type) {
    EastType *t = east_set_type(&east_string_type);
    char buf[128];
    east_type_print(t, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "Set<String>");
    east_type_release(t);
}

TEST(print_dict_type) {
    EastType *t = east_dict_type(&east_string_type, &east_integer_type);
    char buf[128];
    east_type_print(t, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "Dict<String, Integer>");
    east_type_release(t);
}

TEST(print_struct_type) {
    const char *names[] = {"x", "y"};
    EastType *types[] = {&east_integer_type, &east_float_type};
    EastType *t = east_struct_type(names, types, 2);
    char buf[256];
    east_type_print(t, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "Struct { x: Integer, y: Float }");
    east_type_release(t);
}

TEST(print_function_type) {
    EastType *inputs[] = {&east_integer_type, &east_string_type};
    EastType *t = east_function_type(inputs, 2, &east_boolean_type);
    char buf[256];
    east_type_print(t, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "Function(Integer, String) -> Boolean");
    east_type_release(t);
}

TEST(print_null_type_ptr) {
    char buf[64];
    east_type_print(NULL, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "(null)");
}

/* ------------------------------------------------------------------ */
/*  Ref counting                                                       */
/* ------------------------------------------------------------------ */

TEST(refcount_retain_release) {
    EastType *t = east_array_type(&east_integer_type);
    ASSERT_EQ_INT(t->ref_count, 1);

    east_type_retain(t);
    ASSERT_EQ_INT(t->ref_count, 2);

    east_type_retain(t);
    ASSERT_EQ_INT(t->ref_count, 3);

    east_type_release(t);
    ASSERT_EQ_INT(t->ref_count, 2);

    east_type_release(t);
    ASSERT_EQ_INT(t->ref_count, 1);

    /* Final release frees the type. We just verify no crash. */
    east_type_release(t);
}

TEST(refcount_singleton_unaffected) {
    /* Singletons should never have their ref_count changed. */
    int orig = east_integer_type.ref_count;
    east_type_retain(&east_integer_type);
    ASSERT_EQ_INT(east_integer_type.ref_count, orig);

    east_type_release(&east_integer_type);
    ASSERT_EQ_INT(east_integer_type.ref_count, orig);
}

TEST(refcount_null_safe) {
    /* Calling retain/release on NULL must not crash. */
    east_type_retain(NULL);
    east_type_release(NULL);
}

/* ------------------------------------------------------------------ */
/*  Kind name helper                                                   */
/* ------------------------------------------------------------------ */

TEST(kind_name) {
    ASSERT_EQ_STR(east_type_kind_name(EAST_TYPE_INTEGER), "Integer");
    ASSERT_EQ_STR(east_type_kind_name(EAST_TYPE_STRING), "String");
    ASSERT_EQ_STR(east_type_kind_name(EAST_TYPE_ARRAY), "Array");
    ASSERT_EQ_STR(east_type_kind_name(EAST_TYPE_VARIANT), "Variant");
    ASSERT_EQ_STR(east_type_kind_name(EAST_TYPE_FUNCTION), "Function");
    ASSERT_EQ_STR(east_type_kind_name(EAST_TYPE_ASYNC_FUNCTION), "AsyncFunction");
    ASSERT_EQ_STR(east_type_kind_name(EAST_TYPE_RECURSIVE), "Recursive");
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("test_types:\n");

    /* Primitive types */
    RUN_TEST(primitive_kinds);
    RUN_TEST(primitive_singletons_have_negative_refcount);

    /* Parameterized types */
    RUN_TEST(array_type);
    RUN_TEST(set_type);
    RUN_TEST(dict_type);
    RUN_TEST(struct_type);
    RUN_TEST(variant_type);
    RUN_TEST(function_type);
    RUN_TEST(ref_type);
    RUN_TEST(vector_type);
    RUN_TEST(matrix_type);
    RUN_TEST(recursive_type);

    /* Equality */
    RUN_TEST(equal_primitives);
    RUN_TEST(not_equal_different_primitives);
    RUN_TEST(equal_array_types);
    RUN_TEST(not_equal_array_types_diff_elem);
    RUN_TEST(equal_dict_types);
    RUN_TEST(not_equal_dict_types_diff_val);
    RUN_TEST(equal_struct_types);
    RUN_TEST(equal_recursive_types);
    RUN_TEST(not_equal_recursive_types_diff_instance);
    RUN_TEST(equal_null_pointers);

    /* Printing */
    RUN_TEST(print_primitives);
    RUN_TEST(print_array_type);
    RUN_TEST(print_set_type);
    RUN_TEST(print_dict_type);
    RUN_TEST(print_struct_type);
    RUN_TEST(print_function_type);
    RUN_TEST(print_null_type_ptr);

    /* Ref counting */
    RUN_TEST(refcount_retain_release);
    RUN_TEST(refcount_singleton_unaffected);
    RUN_TEST(refcount_null_safe);

    /* Kind name */
    RUN_TEST(kind_name);

    printf("\n  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
