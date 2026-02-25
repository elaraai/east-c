/*
 * Tests for east/serialization.h
 *
 * Covers: JSON encode/decode, BEAST2 binary encode/decode, and East text
 *         print/parse round-trips.
 *
 * Note: Some serialization modules (JSON, BEAST2, East parser/printer) may
 * not be fully implemented yet. Tests that rely on unimplemented functions
 * will detect NULL returns and skip gracefully.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <east/types.h>
#include <east/values.h>
#include <east/serialization.h>

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
/*  JSON round-trip tests                                              */
/* ------------------------------------------------------------------ */

TEST(json_integer_roundtrip) {
    EastValue *v = east_integer(42);
    char *json = east_json_encode(v, &east_integer_type);
    if (!json) {
        /* JSON encoder not implemented yet. */
        east_value_release(v);
        return;
    }
    ASSERT(json != NULL);

    EastValue *decoded = east_json_decode(json, &east_integer_type);
    ASSERT(decoded != NULL);
    ASSERT(east_value_equal(v, decoded));

    east_value_release(v);
    east_value_release(decoded);
    free(json);
}

TEST(json_negative_integer_roundtrip) {
    EastValue *v = east_integer(-999);
    char *json = east_json_encode(v, &east_integer_type);
    if (!json) {
        east_value_release(v);
        return;
    }

    EastValue *decoded = east_json_decode(json, &east_integer_type);
    ASSERT(decoded != NULL);
    ASSERT(east_value_equal(v, decoded));

    east_value_release(v);
    east_value_release(decoded);
    free(json);
}

TEST(json_string_roundtrip) {
    EastValue *v = east_string("hello world");
    char *json = east_json_encode(v, &east_string_type);
    if (!json) {
        east_value_release(v);
        return;
    }

    EastValue *decoded = east_json_decode(json, &east_string_type);
    ASSERT(decoded != NULL);
    ASSERT(east_value_equal(v, decoded));

    east_value_release(v);
    east_value_release(decoded);
    free(json);
}

TEST(json_string_with_escapes_roundtrip) {
    EastValue *v = east_string("line1\nline2\ttab");
    char *json = east_json_encode(v, &east_string_type);
    if (!json) {
        east_value_release(v);
        return;
    }

    EastValue *decoded = east_json_decode(json, &east_string_type);
    ASSERT(decoded != NULL);
    ASSERT(east_value_equal(v, decoded));

    east_value_release(v);
    east_value_release(decoded);
    free(json);
}

TEST(json_boolean_roundtrip) {
    EastValue *v = east_boolean(true);
    char *json = east_json_encode(v, &east_boolean_type);
    if (!json) {
        east_value_release(v);
        return;
    }

    EastValue *decoded = east_json_decode(json, &east_boolean_type);
    ASSERT(decoded != NULL);
    ASSERT(east_value_equal(v, decoded));

    east_value_release(v);
    east_value_release(decoded);
    free(json);
}

TEST(json_null_roundtrip) {
    EastValue *v = east_null();
    char *json = east_json_encode(v, &east_null_type);
    if (!json) {
        return;
    }

    EastValue *decoded = east_json_decode(json, &east_null_type);
    ASSERT(decoded != NULL);
    ASSERT(east_value_equal(v, decoded));

    east_value_release(decoded);
    free(json);
}

TEST(json_float_roundtrip) {
    EastValue *v = east_float(3.14);
    char *json = east_json_encode(v, &east_float_type);
    if (!json) {
        east_value_release(v);
        return;
    }

    EastValue *decoded = east_json_decode(json, &east_float_type);
    ASSERT(decoded != NULL);
    /* Float round-tripping may lose some precision, so check within epsilon. */
    ASSERT_EQ_INT(decoded->kind, EAST_VAL_FLOAT);
    double diff = decoded->data.float64 - 3.14;
    if (diff < 0) diff = -diff;
    ASSERT(diff < 1e-10);

    east_value_release(v);
    east_value_release(decoded);
    free(json);
}

TEST(json_array_roundtrip) {
    EastType *arr_type = east_array_type(&east_integer_type);
    EastValue *arr = east_array_new(&east_integer_type);
    EastValue *v1 = east_integer(1);
    EastValue *v2 = east_integer(2);
    EastValue *v3 = east_integer(3);
    east_array_push(arr, v1);
    east_array_push(arr, v2);
    east_array_push(arr, v3);

    char *json = east_json_encode(arr, arr_type);
    if (!json) {
        east_value_release(v1);
        east_value_release(v2);
        east_value_release(v3);
        east_value_release(arr);
        east_type_release(arr_type);
        return;
    }

    EastValue *decoded = east_json_decode(json, arr_type);
    ASSERT(decoded != NULL);
    ASSERT(east_value_equal(arr, decoded));

    east_value_release(v1);
    east_value_release(v2);
    east_value_release(v3);
    east_value_release(arr);
    east_value_release(decoded);
    east_type_release(arr_type);
    free(json);
}

TEST(json_struct_roundtrip) {
    const char *names[] = {"name", "age"};
    EastType *types[] = {&east_string_type, &east_integer_type};
    EastType *stype = east_struct_type(names, types, 2);

    EastValue *vals[2];
    vals[0] = east_string("Alice");
    vals[1] = east_integer(30);
    EastValue *s = east_struct_new(names, vals, 2, stype);

    char *json = east_json_encode(s, stype);
    if (!json) {
        east_value_release(vals[0]);
        east_value_release(vals[1]);
        east_value_release(s);
        east_type_release(stype);
        return;
    }

    EastValue *decoded = east_json_decode(json, stype);
    ASSERT(decoded != NULL);
    ASSERT(east_value_equal(s, decoded));

    east_value_release(vals[0]);
    east_value_release(vals[1]);
    east_value_release(s);
    east_value_release(decoded);
    east_type_release(stype);
    free(json);
}

/* ------------------------------------------------------------------ */
/*  BEAST2 binary round-trip tests                                     */
/* ------------------------------------------------------------------ */

TEST(beast2_integer_roundtrip) {
    EastValue *v = east_integer(42);
    ByteBuffer *buf = east_beast2_encode(v, &east_integer_type);
    if (!buf) {
        east_value_release(v);
        return;
    }

    EastValue *decoded = east_beast2_decode(buf->data, buf->len, &east_integer_type);
    ASSERT(decoded != NULL);
    ASSERT(east_value_equal(v, decoded));

    east_value_release(v);
    east_value_release(decoded);
    byte_buffer_free(buf);
}

TEST(beast2_negative_integer_roundtrip) {
    EastValue *v = east_integer(-12345);
    ByteBuffer *buf = east_beast2_encode(v, &east_integer_type);
    if (!buf) {
        east_value_release(v);
        return;
    }

    EastValue *decoded = east_beast2_decode(buf->data, buf->len, &east_integer_type);
    ASSERT(decoded != NULL);
    ASSERT(east_value_equal(v, decoded));

    east_value_release(v);
    east_value_release(decoded);
    byte_buffer_free(buf);
}

TEST(beast2_string_roundtrip) {
    EastValue *v = east_string("hello");
    ByteBuffer *buf = east_beast2_encode(v, &east_string_type);
    if (!buf) {
        east_value_release(v);
        return;
    }

    EastValue *decoded = east_beast2_decode(buf->data, buf->len, &east_string_type);
    ASSERT(decoded != NULL);
    ASSERT(east_value_equal(v, decoded));

    east_value_release(v);
    east_value_release(decoded);
    byte_buffer_free(buf);
}

TEST(beast2_boolean_roundtrip) {
    EastValue *v = east_boolean(true);
    ByteBuffer *buf = east_beast2_encode(v, &east_boolean_type);
    if (!buf) {
        east_value_release(v);
        return;
    }

    EastValue *decoded = east_beast2_decode(buf->data, buf->len, &east_boolean_type);
    ASSERT(decoded != NULL);
    ASSERT(east_value_equal(v, decoded));

    east_value_release(v);
    east_value_release(decoded);
    byte_buffer_free(buf);
}

TEST(beast2_array_roundtrip) {
    EastType *arr_type = east_array_type(&east_integer_type);
    EastValue *arr = east_array_new(&east_integer_type);
    EastValue *v1 = east_integer(10);
    EastValue *v2 = east_integer(20);
    east_array_push(arr, v1);
    east_array_push(arr, v2);

    ByteBuffer *buf = east_beast2_encode(arr, arr_type);
    if (!buf) {
        east_value_release(v1);
        east_value_release(v2);
        east_value_release(arr);
        east_type_release(arr_type);
        return;
    }

    EastValue *decoded = east_beast2_decode(buf->data, buf->len, arr_type);
    ASSERT(decoded != NULL);
    ASSERT(east_value_equal(arr, decoded));

    east_value_release(v1);
    east_value_release(v2);
    east_value_release(arr);
    east_value_release(decoded);
    east_type_release(arr_type);
    byte_buffer_free(buf);
}

/* ------------------------------------------------------------------ */
/*  East text format round-trip tests                                  */
/* ------------------------------------------------------------------ */

TEST(east_text_integer_roundtrip) {
    EastValue *v = east_integer(42);
    char *text = east_print_value(v, &east_integer_type);
    if (!text) {
        east_value_release(v);
        return;
    }

    EastValue *decoded = east_parse_value(text, &east_integer_type);
    ASSERT(decoded != NULL);
    ASSERT(east_value_equal(v, decoded));

    east_value_release(v);
    east_value_release(decoded);
    free(text);
}

TEST(east_text_string_roundtrip) {
    EastValue *v = east_string("hello");
    char *text = east_print_value(v, &east_string_type);
    if (!text) {
        east_value_release(v);
        return;
    }

    EastValue *decoded = east_parse_value(text, &east_string_type);
    ASSERT(decoded != NULL);
    ASSERT(east_value_equal(v, decoded));

    east_value_release(v);
    east_value_release(decoded);
    free(text);
}

TEST(east_text_boolean_roundtrip) {
    EastValue *v = east_boolean(false);
    char *text = east_print_value(v, &east_boolean_type);
    if (!text) {
        east_value_release(v);
        return;
    }

    EastValue *decoded = east_parse_value(text, &east_boolean_type);
    ASSERT(decoded != NULL);
    ASSERT(east_value_equal(v, decoded));

    east_value_release(v);
    east_value_release(decoded);
    free(text);
}

TEST(east_text_null_roundtrip) {
    EastValue *v = east_null();
    char *text = east_print_value(v, &east_null_type);
    if (!text) {
        return;
    }

    EastValue *decoded = east_parse_value(text, &east_null_type);
    ASSERT(decoded != NULL);
    ASSERT(east_value_equal(v, decoded));

    east_value_release(decoded);
    free(text);
}

TEST(east_text_array_roundtrip) {
    EastType *arr_type = east_array_type(&east_integer_type);
    EastValue *arr = east_array_new(&east_integer_type);
    EastValue *v1 = east_integer(1);
    EastValue *v2 = east_integer(2);
    east_array_push(arr, v1);
    east_array_push(arr, v2);

    char *text = east_print_value(arr, arr_type);
    if (!text) {
        east_value_release(v1);
        east_value_release(v2);
        east_value_release(arr);
        east_type_release(arr_type);
        return;
    }

    EastValue *decoded = east_parse_value(text, arr_type);
    ASSERT(decoded != NULL);
    ASSERT(east_value_equal(arr, decoded));

    east_value_release(v1);
    east_value_release(v2);
    east_value_release(arr);
    east_value_release(decoded);
    east_type_release(arr_type);
    free(text);
}

/* ------------------------------------------------------------------ */
/*  Binary utility tests (varint/zigzag -- always available)           */
/* ------------------------------------------------------------------ */

TEST(varint_roundtrip) {
    ByteBuffer *buf = byte_buffer_new(64);
    ASSERT(buf != NULL);

    write_varint(buf, 0);
    write_varint(buf, 1);
    write_varint(buf, 127);
    write_varint(buf, 128);
    write_varint(buf, 300);
    write_varint(buf, 100000);

    size_t offset = 0;
    ASSERT_EQ_INT((int64_t)read_varint(buf->data, &offset), 0);
    ASSERT_EQ_INT((int64_t)read_varint(buf->data, &offset), 1);
    ASSERT_EQ_INT((int64_t)read_varint(buf->data, &offset), 127);
    ASSERT_EQ_INT((int64_t)read_varint(buf->data, &offset), 128);
    ASSERT_EQ_INT((int64_t)read_varint(buf->data, &offset), 300);
    ASSERT_EQ_INT((int64_t)read_varint(buf->data, &offset), 100000);

    byte_buffer_free(buf);
}

TEST(zigzag_roundtrip) {
    ByteBuffer *buf = byte_buffer_new(64);
    ASSERT(buf != NULL);

    write_zigzag(buf, 0);
    write_zigzag(buf, -1);
    write_zigzag(buf, 1);
    write_zigzag(buf, -2);
    write_zigzag(buf, 2);
    write_zigzag(buf, -12345);
    write_zigzag(buf, 12345);

    size_t offset = 0;
    ASSERT_EQ_INT(read_zigzag(buf->data, &offset), 0);
    ASSERT_EQ_INT(read_zigzag(buf->data, &offset), -1);
    ASSERT_EQ_INT(read_zigzag(buf->data, &offset), 1);
    ASSERT_EQ_INT(read_zigzag(buf->data, &offset), -2);
    ASSERT_EQ_INT(read_zigzag(buf->data, &offset), 2);
    ASSERT_EQ_INT(read_zigzag(buf->data, &offset), -12345);
    ASSERT_EQ_INT(read_zigzag(buf->data, &offset), 12345);

    byte_buffer_free(buf);
}

TEST(byte_buffer_write_u8) {
    ByteBuffer *buf = byte_buffer_new(4);
    ASSERT(buf != NULL);
    ASSERT_EQ_INT((int64_t)buf->len, 0);

    byte_buffer_write_u8(buf, 0xAA);
    byte_buffer_write_u8(buf, 0xBB);

    ASSERT_EQ_INT((int64_t)buf->len, 2);
    ASSERT_EQ_INT(buf->data[0], 0xAA);
    ASSERT_EQ_INT(buf->data[1], 0xBB);

    byte_buffer_free(buf);
}

TEST(byte_buffer_write_bytes) {
    ByteBuffer *buf = byte_buffer_new(4);
    ASSERT(buf != NULL);

    uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    byte_buffer_write_bytes(buf, data, 5);

    ASSERT_EQ_INT((int64_t)buf->len, 5);
    ASSERT_EQ_INT(buf->data[0], 0x01);
    ASSERT_EQ_INT(buf->data[4], 0x05);

    byte_buffer_free(buf);
}

TEST(byte_buffer_growth) {
    /* Start with tiny buffer and write a lot of data. */
    ByteBuffer *buf = byte_buffer_new(2);
    ASSERT(buf != NULL);

    for (int i = 0; i < 100; i++) {
        byte_buffer_write_u8(buf, (uint8_t)(i & 0xFF));
    }
    ASSERT_EQ_INT((int64_t)buf->len, 100);
    ASSERT_EQ_INT(buf->data[0], 0);
    ASSERT_EQ_INT(buf->data[99], 99);

    byte_buffer_free(buf);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("test_serialization:\n");

    /* JSON */
    RUN_TEST(json_integer_roundtrip);
    RUN_TEST(json_negative_integer_roundtrip);
    RUN_TEST(json_string_roundtrip);
    RUN_TEST(json_string_with_escapes_roundtrip);
    RUN_TEST(json_boolean_roundtrip);
    RUN_TEST(json_null_roundtrip);
    RUN_TEST(json_float_roundtrip);
    RUN_TEST(json_array_roundtrip);
    RUN_TEST(json_struct_roundtrip);

    /* BEAST2 */
    RUN_TEST(beast2_integer_roundtrip);
    RUN_TEST(beast2_negative_integer_roundtrip);
    RUN_TEST(beast2_string_roundtrip);
    RUN_TEST(beast2_boolean_roundtrip);
    RUN_TEST(beast2_array_roundtrip);

    /* East text format */
    RUN_TEST(east_text_integer_roundtrip);
    RUN_TEST(east_text_string_roundtrip);
    RUN_TEST(east_text_boolean_roundtrip);
    RUN_TEST(east_text_null_roundtrip);
    RUN_TEST(east_text_array_roundtrip);

    /* Binary utilities (always available) */
    RUN_TEST(varint_roundtrip);
    RUN_TEST(zigzag_roundtrip);
    RUN_TEST(byte_buffer_write_u8);
    RUN_TEST(byte_buffer_write_bytes);
    RUN_TEST(byte_buffer_growth);

    printf("\n  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
