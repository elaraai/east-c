#ifndef EAST_SERIALIZATION_H
#define EAST_SERIALIZATION_H

#include "types.h"
#include "values.h"
#include <stddef.h>
#include <stdint.h>

// JSON serialization
char *east_json_encode(EastValue *value, EastType *type);
EastValue *east_json_decode(const char *json, EastType *type);
// JSON decode with detailed error message (caller frees *error_out on failure)
EastValue *east_json_decode_with_error(const char *json, EastType *type, char **error_out);

// Byte buffer for binary serialization
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} ByteBuffer;

ByteBuffer *byte_buffer_new(size_t initial_cap);
void byte_buffer_free(ByteBuffer *buf);
void byte_buffer_write_u8(ByteBuffer *buf, uint8_t val);
void byte_buffer_write_bytes(ByteBuffer *buf, const uint8_t *data, size_t len);

// BEAST2 binary serialization (headerless, type-driven)
ByteBuffer *east_beast2_encode(EastValue *value, EastType *type);
EastValue *east_beast2_decode(const uint8_t *data, size_t len, EastType *type);

// BEAST2 with header (magic bytes + type schema + value)
ByteBuffer *east_beast2_encode_full(EastValue *value, EastType *type);
EastValue *east_beast2_decode_full(const uint8_t *data, size_t len, EastType *type);

// Beast v1 binary serialization (magic + type schema + twiddled values)
ByteBuffer *east_beast_encode(EastValue *value, EastType *type);
EastValue *east_beast_decode(const uint8_t *data, size_t len, EastType *type);

// CSV serialization
// config may be NULL for defaults, or an EastValue struct with Option fields
char *east_csv_encode(EastValue *array, EastType *type, EastValue *config);
EastValue *east_csv_decode(const char *csv, EastType *type, EastValue *config);

// East text format
char *east_print_value(EastValue *value, EastType *type);
EastValue *east_parse_value(const char *text, EastType *type);
// East parse with detailed error message (caller frees *error_out on failure)
EastValue *east_parse_value_with_error(const char *text, EastType *type, char **error_out);
char *east_print_type(EastType *type);
EastType *east_parse_type(const char *text);

// Binary utilities
void write_varint(ByteBuffer *buf, uint64_t val);
uint64_t read_varint(const uint8_t *data, size_t *offset);
void write_zigzag(ByteBuffer *buf, int64_t val);
int64_t read_zigzag(const uint8_t *data, size_t *offset);

#endif
