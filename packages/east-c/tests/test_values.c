/*
 * Tests for east/values.h
 *
 * Covers: value construction, comparison, equality, ref counting,
 *         collections, structs, variants, and printing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <east/types.h>
#include <east/values.h>

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
/*  Constructors                                                       */
/* ------------------------------------------------------------------ */

TEST(null_value) {
    EastValue *v = east_null();
    ASSERT(v != NULL);
    ASSERT_EQ_INT(v->kind, EAST_VAL_NULL);
    /* Null is a singleton, should be the same pointer every time. */
    ASSERT(v == &east_null_value);
    /* Don't release: it is a singleton. */
}

TEST(boolean_true) {
    EastValue *v = east_boolean(true);
    ASSERT(v != NULL);
    ASSERT_EQ_INT(v->kind, EAST_VAL_BOOLEAN);
    ASSERT(v->data.boolean == true);
    east_value_release(v);
}

TEST(boolean_false) {
    EastValue *v = east_boolean(false);
    ASSERT(v != NULL);
    ASSERT(v->data.boolean == false);
    east_value_release(v);
}

TEST(integer_value) {
    EastValue *v = east_integer(42);
    ASSERT(v != NULL);
    ASSERT_EQ_INT(v->kind, EAST_VAL_INTEGER);
    ASSERT_EQ_INT(v->data.integer, 42);
    east_value_release(v);
}

TEST(integer_negative) {
    EastValue *v = east_integer(-123);
    ASSERT(v != NULL);
    ASSERT_EQ_INT(v->data.integer, -123);
    east_value_release(v);
}

TEST(integer_zero) {
    EastValue *v = east_integer(0);
    ASSERT(v != NULL);
    ASSERT_EQ_INT(v->data.integer, 0);
    east_value_release(v);
}

TEST(float_value) {
    EastValue *v = east_float(3.14);
    ASSERT(v != NULL);
    ASSERT_EQ_INT(v->kind, EAST_VAL_FLOAT);
    ASSERT(v->data.float64 == 3.14);
    east_value_release(v);
}

TEST(string_value) {
    EastValue *v = east_string("hello");
    ASSERT(v != NULL);
    ASSERT_EQ_INT(v->kind, EAST_VAL_STRING);
    ASSERT_EQ_STR(v->data.string.data, "hello");
    ASSERT_EQ_INT((int64_t)v->data.string.len, 5);
    east_value_release(v);
}

TEST(string_empty) {
    EastValue *v = east_string("");
    ASSERT(v != NULL);
    ASSERT_EQ_INT((int64_t)v->data.string.len, 0);
    ASSERT_EQ_STR(v->data.string.data, "");
    east_value_release(v);
}

TEST(string_null_arg) {
    /* east_string(NULL) should treat it as empty string. */
    EastValue *v = east_string(NULL);
    ASSERT(v != NULL);
    ASSERT_EQ_INT(v->kind, EAST_VAL_STRING);
    ASSERT_EQ_INT((int64_t)v->data.string.len, 0);
    east_value_release(v);
}

TEST(string_len) {
    EastValue *v = east_string_len("hello world", 5);
    ASSERT(v != NULL);
    ASSERT_EQ_INT((int64_t)v->data.string.len, 5);
    ASSERT_EQ_STR(v->data.string.data, "hello");
    east_value_release(v);
}

TEST(datetime_value) {
    EastValue *v = east_datetime(1700000000000LL);
    ASSERT(v != NULL);
    ASSERT_EQ_INT(v->kind, EAST_VAL_DATETIME);
    ASSERT_EQ_INT(v->data.datetime, 1700000000000LL);
    east_value_release(v);
}

TEST(blob_value) {
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    EastValue *v = east_blob(data, 4);
    ASSERT(v != NULL);
    ASSERT_EQ_INT(v->kind, EAST_VAL_BLOB);
    ASSERT_EQ_INT((int64_t)v->data.blob.len, 4);
    ASSERT(v->data.blob.data[0] == 0xDE);
    ASSERT(v->data.blob.data[3] == 0xEF);
    east_value_release(v);
}

/* ------------------------------------------------------------------ */
/*  Equality                                                           */
/* ------------------------------------------------------------------ */

TEST(equal_nulls) {
    EastValue *a = east_null();
    EastValue *b = east_null();
    ASSERT(east_value_equal(a, b));
}

TEST(equal_booleans) {
    EastValue *a = east_boolean(true);
    EastValue *b = east_boolean(true);
    ASSERT(east_value_equal(a, b));
    east_value_release(a);
    east_value_release(b);
}

TEST(not_equal_booleans) {
    EastValue *a = east_boolean(true);
    EastValue *b = east_boolean(false);
    ASSERT(!east_value_equal(a, b));
    east_value_release(a);
    east_value_release(b);
}

TEST(equal_integers) {
    EastValue *a = east_integer(100);
    EastValue *b = east_integer(100);
    ASSERT(east_value_equal(a, b));
    east_value_release(a);
    east_value_release(b);
}

TEST(not_equal_integers) {
    EastValue *a = east_integer(1);
    EastValue *b = east_integer(2);
    ASSERT(!east_value_equal(a, b));
    east_value_release(a);
    east_value_release(b);
}

TEST(equal_floats) {
    EastValue *a = east_float(2.718);
    EastValue *b = east_float(2.718);
    ASSERT(east_value_equal(a, b));
    east_value_release(a);
    east_value_release(b);
}

TEST(equal_strings) {
    EastValue *a = east_string("hello");
    EastValue *b = east_string("hello");
    ASSERT(east_value_equal(a, b));
    east_value_release(a);
    east_value_release(b);
}

TEST(not_equal_strings) {
    EastValue *a = east_string("hello");
    EastValue *b = east_string("world");
    ASSERT(!east_value_equal(a, b));
    east_value_release(a);
    east_value_release(b);
}

TEST(not_equal_different_kinds) {
    EastValue *a = east_integer(1);
    EastValue *b = east_string("1");
    ASSERT(!east_value_equal(a, b));
    east_value_release(a);
    east_value_release(b);
}

/* ------------------------------------------------------------------ */
/*  Comparison ordering                                                */
/* ------------------------------------------------------------------ */

TEST(compare_integers_ascending) {
    EastValue *a = east_integer(1);
    EastValue *b = east_integer(2);
    ASSERT(east_value_compare(a, b) < 0);
    ASSERT(east_value_compare(b, a) > 0);
    east_value_release(a);
    east_value_release(b);
}

TEST(compare_integers_equal) {
    EastValue *a = east_integer(5);
    EastValue *b = east_integer(5);
    ASSERT_EQ_INT(east_value_compare(a, b), 0);
    east_value_release(a);
    east_value_release(b);
}

TEST(compare_strings_ascending) {
    EastValue *a = east_string("abc");
    EastValue *b = east_string("abd");
    ASSERT(east_value_compare(a, b) < 0);
    east_value_release(a);
    east_value_release(b);
}

TEST(compare_booleans) {
    EastValue *f = east_boolean(false);
    EastValue *t = east_boolean(true);
    /* false < true */
    ASSERT(east_value_compare(f, t) < 0);
    ASSERT(east_value_compare(t, f) > 0);
    east_value_release(f);
    east_value_release(t);
}

TEST(compare_different_kinds) {
    /* null < integer (kind rank ordering) */
    EastValue *n = east_null();
    EastValue *i = east_integer(0);
    ASSERT(east_value_compare(n, i) < 0);
    east_value_release(i);
}

TEST(compare_null_pointers) {
    EastValue *v = east_integer(1);
    ASSERT(east_value_compare(NULL, v) < 0);
    ASSERT(east_value_compare(v, NULL) > 0);
    ASSERT_EQ_INT(east_value_compare(NULL, NULL), 0);
    east_value_release(v);
}

/* ------------------------------------------------------------------ */
/*  Arrays                                                             */
/* ------------------------------------------------------------------ */

TEST(array_create_and_push) {
    EastValue *arr = east_array_new(&east_integer_type);
    ASSERT(arr != NULL);
    ASSERT_EQ_INT(arr->kind, EAST_VAL_ARRAY);
    ASSERT_EQ_INT((int64_t)east_array_len(arr), 0);

    EastValue *v1 = east_integer(10);
    EastValue *v2 = east_integer(20);
    EastValue *v3 = east_integer(30);
    east_array_push(arr, v1);
    east_array_push(arr, v2);
    east_array_push(arr, v3);

    ASSERT_EQ_INT((int64_t)east_array_len(arr), 3);

    EastValue *g0 = east_array_get(arr, 0);
    EastValue *g1 = east_array_get(arr, 1);
    EastValue *g2 = east_array_get(arr, 2);
    ASSERT(g0 != NULL);
    ASSERT_EQ_INT(g0->data.integer, 10);
    ASSERT_EQ_INT(g1->data.integer, 20);
    ASSERT_EQ_INT(g2->data.integer, 30);

    /* Out of bounds */
    ASSERT(east_array_get(arr, 3) == NULL);

    east_value_release(v1);
    east_value_release(v2);
    east_value_release(v3);
    east_value_release(arr);
}

TEST(array_equality) {
    EastValue *a = east_array_new(&east_integer_type);
    EastValue *b = east_array_new(&east_integer_type);

    EastValue *v1 = east_integer(1);
    EastValue *v2 = east_integer(2);

    east_array_push(a, v1);
    east_array_push(a, v2);
    east_array_push(b, v1);
    east_array_push(b, v2);

    ASSERT(east_value_equal(a, b));

    east_value_release(v1);
    east_value_release(v2);
    east_value_release(a);
    east_value_release(b);
}

TEST(array_grow_beyond_initial_capacity) {
    EastValue *arr = east_array_new(&east_integer_type);
    /* Initial capacity is 4, push 10 items to force growth. */
    for (int i = 0; i < 10; i++) {
        EastValue *v = east_integer(i);
        east_array_push(arr, v);
        east_value_release(v);
    }
    ASSERT_EQ_INT((int64_t)east_array_len(arr), 10);
    EastValue *last = east_array_get(arr, 9);
    ASSERT_EQ_INT(last->data.integer, 9);
    east_value_release(arr);
}

/* ------------------------------------------------------------------ */
/*  Sets                                                               */
/* ------------------------------------------------------------------ */

TEST(set_insert_and_sorted) {
    EastValue *s = east_set_new(&east_integer_type);
    ASSERT(s != NULL);
    ASSERT_EQ_INT((int64_t)east_set_len(s), 0);

    EastValue *v3 = east_integer(30);
    EastValue *v1 = east_integer(10);
    EastValue *v2 = east_integer(20);

    east_set_insert(s, v3);
    east_set_insert(s, v1);
    east_set_insert(s, v2);

    ASSERT_EQ_INT((int64_t)east_set_len(s), 3);

    /* Items should be sorted: 10, 20, 30 */
    ASSERT_EQ_INT(s->data.set.items[0]->data.integer, 10);
    ASSERT_EQ_INT(s->data.set.items[1]->data.integer, 20);
    ASSERT_EQ_INT(s->data.set.items[2]->data.integer, 30);

    east_value_release(v1);
    east_value_release(v2);
    east_value_release(v3);
    east_value_release(s);
}

TEST(set_dedup) {
    EastValue *s = east_set_new(&east_integer_type);
    EastValue *v1 = east_integer(5);
    EastValue *v2 = east_integer(5);

    east_set_insert(s, v1);
    east_set_insert(s, v2);

    ASSERT_EQ_INT((int64_t)east_set_len(s), 1);

    east_value_release(v1);
    east_value_release(v2);
    east_value_release(s);
}

TEST(set_has) {
    EastValue *s = east_set_new(&east_integer_type);
    EastValue *v1 = east_integer(42);
    EastValue *v2 = east_integer(99);

    east_set_insert(s, v1);

    ASSERT(east_set_has(s, v1));
    ASSERT(!east_set_has(s, v2));

    east_value_release(v1);
    east_value_release(v2);
    east_value_release(s);
}

/* ------------------------------------------------------------------ */
/*  Dicts                                                              */
/* ------------------------------------------------------------------ */

TEST(dict_set_get) {
    EastValue *d = east_dict_new(&east_string_type, &east_integer_type);
    ASSERT(d != NULL);
    ASSERT_EQ_INT((int64_t)east_dict_len(d), 0);

    EastValue *k1 = east_string("alpha");
    EastValue *v1 = east_integer(1);
    EastValue *k2 = east_string("beta");
    EastValue *v2 = east_integer(2);

    east_dict_set(d, k1, v1);
    east_dict_set(d, k2, v2);

    ASSERT_EQ_INT((int64_t)east_dict_len(d), 2);

    EastValue *got1 = east_dict_get(d, k1);
    EastValue *got2 = east_dict_get(d, k2);
    ASSERT(got1 != NULL);
    ASSERT_EQ_INT(got1->data.integer, 1);
    ASSERT(got2 != NULL);
    ASSERT_EQ_INT(got2->data.integer, 2);

    /* Has */
    ASSERT(east_dict_has(d, k1));

    /* Missing key */
    EastValue *k3 = east_string("gamma");
    ASSERT(!east_dict_has(d, k3));
    ASSERT(east_dict_get(d, k3) == NULL);

    east_value_release(k1);
    east_value_release(v1);
    east_value_release(k2);
    east_value_release(v2);
    east_value_release(k3);
    east_value_release(d);
}

TEST(dict_overwrite) {
    EastValue *d = east_dict_new(&east_string_type, &east_integer_type);
    EastValue *k = east_string("key");
    EastValue *v1 = east_integer(100);
    EastValue *v2 = east_integer(200);

    east_dict_set(d, k, v1);
    ASSERT_EQ_INT(east_dict_get(d, k)->data.integer, 100);

    east_dict_set(d, k, v2);
    ASSERT_EQ_INT(east_dict_get(d, k)->data.integer, 200);
    ASSERT_EQ_INT((int64_t)east_dict_len(d), 1);

    east_value_release(k);
    east_value_release(v1);
    east_value_release(v2);
    east_value_release(d);
}

/* ------------------------------------------------------------------ */
/*  Structs                                                            */
/* ------------------------------------------------------------------ */

TEST(struct_create_and_get_field) {
    const char *names[] = {"name", "age"};
    EastValue *vals[2];
    vals[0] = east_string("Alice");
    vals[1] = east_integer(30);

    const char *tnames[] = {"name", "age"};
    EastType *ttypes[] = {&east_string_type, &east_integer_type};
    EastType *stype = east_struct_type(tnames, ttypes, 2);

    EastValue *s = east_struct_new(names, vals, 2, stype);
    ASSERT(s != NULL);
    ASSERT_EQ_INT(s->kind, EAST_VAL_STRUCT);
    ASSERT_EQ_INT((int64_t)s->data.struct_.num_fields, 2);

    EastValue *name_field = east_struct_get_field(s, "name");
    ASSERT(name_field != NULL);
    ASSERT_EQ_STR(name_field->data.string.data, "Alice");

    EastValue *age_field = east_struct_get_field(s, "age");
    ASSERT(age_field != NULL);
    ASSERT_EQ_INT(age_field->data.integer, 30);

    /* Non-existent field */
    ASSERT(east_struct_get_field(s, "missing") == NULL);

    east_value_release(vals[0]);
    east_value_release(vals[1]);
    east_type_release(stype);
    east_value_release(s);
}

/* ------------------------------------------------------------------ */
/*  Variants                                                           */
/* ------------------------------------------------------------------ */

TEST(variant_create) {
    EastValue *inner = east_integer(42);
    EastValue *v = east_variant_new("Some", inner, NULL);
    ASSERT(v != NULL);
    ASSERT_EQ_INT(v->kind, EAST_VAL_VARIANT);
    ASSERT_EQ_STR(v->data.variant.case_name, "Some");
    ASSERT(v->data.variant.value != NULL);
    ASSERT_EQ_INT(v->data.variant.value->data.integer, 42);
    east_value_release(inner);
    east_value_release(v);
}

TEST(variant_equality) {
    EastValue *inner1 = east_integer(1);
    EastValue *inner2 = east_integer(1);
    EastValue *a = east_variant_new("Some", inner1, NULL);
    EastValue *b = east_variant_new("Some", inner2, NULL);
    ASSERT(east_value_equal(a, b));

    EastValue *c = east_variant_new("None", east_null(), NULL);
    ASSERT(!east_value_equal(a, c));

    east_value_release(inner1);
    east_value_release(inner2);
    east_value_release(a);
    east_value_release(b);
    east_value_release(c);
}

/* ------------------------------------------------------------------ */
/*  Ref values                                                         */
/* ------------------------------------------------------------------ */

TEST(ref_create_get_set) {
    EastValue *inner = east_integer(10);
    EastValue *r = east_ref_new(inner);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(r->kind, EAST_VAL_REF);

    EastValue *got = east_ref_get(r);
    ASSERT(got != NULL);
    ASSERT_EQ_INT(got->data.integer, 10);

    EastValue *new_val = east_integer(20);
    east_ref_set(r, new_val);
    got = east_ref_get(r);
    ASSERT_EQ_INT(got->data.integer, 20);

    east_value_release(inner);
    east_value_release(new_val);
    east_value_release(r);
}

/* ------------------------------------------------------------------ */
/*  Ref counting                                                       */
/* ------------------------------------------------------------------ */

TEST(refcount_basic) {
    EastValue *v = east_integer(42);
    ASSERT_EQ_INT(v->ref_count, 1);

    east_value_retain(v);
    ASSERT_EQ_INT(v->ref_count, 2);

    east_value_release(v);
    ASSERT_EQ_INT(v->ref_count, 1);

    /* Final release frees the value. */
    east_value_release(v);
}

TEST(refcount_null_singleton) {
    /* Null singleton has negative refcount and retain/release are no-ops. */
    EastValue *n = east_null();
    int orig = n->ref_count;
    ASSERT(orig < 0);

    east_value_retain(n);
    ASSERT_EQ_INT(n->ref_count, orig);

    east_value_release(n);
    ASSERT_EQ_INT(n->ref_count, orig);
}

TEST(refcount_null_ptr_safe) {
    /* Must not crash. */
    east_value_retain(NULL);
    east_value_release(NULL);
}

TEST(refcount_array_retains_items) {
    EastValue *arr = east_array_new(&east_integer_type);
    EastValue *v = east_integer(7);
    ASSERT_EQ_INT(v->ref_count, 1);

    east_array_push(arr, v);
    /* array_push retains the item */
    ASSERT_EQ_INT(v->ref_count, 2);

    east_value_release(v);
    ASSERT_EQ_INT(v->ref_count, 1);

    /* Releasing the array releases its items. */
    east_value_release(arr);
    /* v is now freed -- we cannot check ref_count, just verify no crash. */
}

/* ------------------------------------------------------------------ */
/*  Printing                                                           */
/* ------------------------------------------------------------------ */

TEST(print_null) {
    char buf[64];
    east_value_print(east_null(), buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "null");
}

TEST(print_boolean) {
    char buf[64];
    EastValue *t = east_boolean(true);
    EastValue *f = east_boolean(false);
    east_value_print(t, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "true");
    east_value_print(f, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "false");
    east_value_release(t);
    east_value_release(f);
}

TEST(print_integer) {
    char buf[64];
    EastValue *v = east_integer(12345);
    east_value_print(v, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "12345");
    east_value_release(v);
}

TEST(print_negative_integer) {
    char buf[64];
    EastValue *v = east_integer(-99);
    east_value_print(v, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "-99");
    east_value_release(v);
}

TEST(print_string) {
    char buf[64];
    EastValue *v = east_string("hello");
    east_value_print(v, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "\"hello\"");
    east_value_release(v);
}

TEST(print_string_with_escapes) {
    char buf[128];
    EastValue *v = east_string("line1\nline2");
    east_value_print(v, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "\"line1\\nline2\"");
    east_value_release(v);
}

TEST(print_array) {
    char buf[128];
    EastValue *arr = east_array_new(&east_integer_type);
    EastValue *v1 = east_integer(1);
    EastValue *v2 = east_integer(2);
    EastValue *v3 = east_integer(3);
    east_array_push(arr, v1);
    east_array_push(arr, v2);
    east_array_push(arr, v3);
    east_value_print(arr, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "[1, 2, 3]");
    east_value_release(v1);
    east_value_release(v2);
    east_value_release(v3);
    east_value_release(arr);
}

TEST(print_empty_array) {
    char buf[64];
    EastValue *arr = east_array_new(&east_integer_type);
    east_value_print(arr, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "[]");
    east_value_release(arr);
}

TEST(print_struct) {
    char buf[256];
    const char *names[] = {"x", "y"};
    EastValue *vals[2];
    vals[0] = east_integer(10);
    vals[1] = east_integer(20);
    EastValue *s = east_struct_new(names, vals, 2, NULL);
    east_value_print(s, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "{x: 10, y: 20}");
    east_value_release(vals[0]);
    east_value_release(vals[1]);
    east_value_release(s);
}

TEST(print_variant) {
    char buf[128];
    EastValue *inner = east_integer(42);
    EastValue *v = east_variant_new("Some", inner, NULL);
    east_value_print(v, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, ".Some 42");
    east_value_release(inner);
    east_value_release(v);
}

TEST(print_variant_null_payload) {
    char buf[128];
    EastValue *v = east_variant_new("None", east_null(), NULL);
    east_value_print(v, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, ".None");
    east_value_release(v);
}

TEST(print_dict) {
    char buf[256];
    EastValue *d = east_dict_new(&east_string_type, &east_integer_type);
    EastValue *k1 = east_string("a");
    EastValue *v1 = east_integer(1);
    east_dict_set(d, k1, v1);
    east_value_print(d, buf, sizeof(buf));
    ASSERT_EQ_STR(buf, "{\"a\": 1}");
    east_value_release(k1);
    east_value_release(v1);
    east_value_release(d);
}

/* ------------------------------------------------------------------ */
/*  Kind name                                                          */
/* ------------------------------------------------------------------ */

TEST(kind_name) {
    ASSERT_EQ_STR(east_value_kind_name(EAST_VAL_NULL), "Null");
    ASSERT_EQ_STR(east_value_kind_name(EAST_VAL_INTEGER), "Integer");
    ASSERT_EQ_STR(east_value_kind_name(EAST_VAL_STRING), "String");
    ASSERT_EQ_STR(east_value_kind_name(EAST_VAL_ARRAY), "Array");
    ASSERT_EQ_STR(east_value_kind_name(EAST_VAL_FUNCTION), "Function");
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("test_values:\n");

    /* Constructors */
    RUN_TEST(null_value);
    RUN_TEST(boolean_true);
    RUN_TEST(boolean_false);
    RUN_TEST(integer_value);
    RUN_TEST(integer_negative);
    RUN_TEST(integer_zero);
    RUN_TEST(float_value);
    RUN_TEST(string_value);
    RUN_TEST(string_empty);
    RUN_TEST(string_null_arg);
    RUN_TEST(string_len);
    RUN_TEST(datetime_value);
    RUN_TEST(blob_value);

    /* Equality */
    RUN_TEST(equal_nulls);
    RUN_TEST(equal_booleans);
    RUN_TEST(not_equal_booleans);
    RUN_TEST(equal_integers);
    RUN_TEST(not_equal_integers);
    RUN_TEST(equal_floats);
    RUN_TEST(equal_strings);
    RUN_TEST(not_equal_strings);
    RUN_TEST(not_equal_different_kinds);

    /* Comparison ordering */
    RUN_TEST(compare_integers_ascending);
    RUN_TEST(compare_integers_equal);
    RUN_TEST(compare_strings_ascending);
    RUN_TEST(compare_booleans);
    RUN_TEST(compare_different_kinds);
    RUN_TEST(compare_null_pointers);

    /* Collections */
    RUN_TEST(array_create_and_push);
    RUN_TEST(array_equality);
    RUN_TEST(array_grow_beyond_initial_capacity);
    RUN_TEST(set_insert_and_sorted);
    RUN_TEST(set_dedup);
    RUN_TEST(set_has);
    RUN_TEST(dict_set_get);
    RUN_TEST(dict_overwrite);

    /* Structs */
    RUN_TEST(struct_create_and_get_field);

    /* Variants */
    RUN_TEST(variant_create);
    RUN_TEST(variant_equality);

    /* Refs */
    RUN_TEST(ref_create_get_set);

    /* Ref counting */
    RUN_TEST(refcount_basic);
    RUN_TEST(refcount_null_singleton);
    RUN_TEST(refcount_null_ptr_safe);
    RUN_TEST(refcount_array_retains_items);

    /* Printing */
    RUN_TEST(print_null);
    RUN_TEST(print_boolean);
    RUN_TEST(print_integer);
    RUN_TEST(print_negative_integer);
    RUN_TEST(print_string);
    RUN_TEST(print_string_with_escapes);
    RUN_TEST(print_array);
    RUN_TEST(print_empty_array);
    RUN_TEST(print_struct);
    RUN_TEST(print_variant);
    RUN_TEST(print_variant_null_payload);
    RUN_TEST(print_dict);

    /* Kind name */
    RUN_TEST(kind_name);

    printf("\n  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
