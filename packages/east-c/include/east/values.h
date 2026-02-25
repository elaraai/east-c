#ifndef EAST_VALUES_H
#define EAST_VALUES_H

#include "types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    EAST_VAL_NULL,
    EAST_VAL_BOOLEAN,
    EAST_VAL_INTEGER,
    EAST_VAL_FLOAT,
    EAST_VAL_STRING,
    EAST_VAL_DATETIME,
    EAST_VAL_BLOB,
    EAST_VAL_ARRAY,
    EAST_VAL_SET,
    EAST_VAL_DICT,
    EAST_VAL_STRUCT,
    EAST_VAL_VARIANT,
    EAST_VAL_REF,
    EAST_VAL_VECTOR,
    EAST_VAL_MATRIX,
    EAST_VAL_FUNCTION,
} EastValueKind;

typedef struct EastValue EastValue;
typedef struct EastCompiledFn EastCompiledFn;

struct EastValue {
    EastValueKind kind;
    int ref_count;

    /* GC cycle-collector tracking (used only for container types) */
    struct EastValue *gc_next;
    struct EastValue *gc_prev;
    int gc_refs;           /* temporary refcount during collection */
    bool gc_tracked;       /* true if in GC tracking list */

    union {
        bool boolean;
        int64_t integer;
        double float64;
        struct { char *data; size_t len; } string;
        int64_t datetime;  // epoch millis
        struct { uint8_t *data; size_t len; } blob;
        struct {
            EastValue **items;
            size_t len;
            size_t cap;
            EastType *elem_type;
        } array;
        struct {
            EastValue **items;
            size_t len;
            size_t cap;
            EastType *elem_type;
        } set;
        struct {
            EastValue **keys;
            EastValue **values;
            size_t len;
            size_t cap;
            EastType *key_type;
            EastType *val_type;
        } dict;
        struct {
            char **field_names;
            EastValue **field_values;
            size_t num_fields;
            EastType *type;
        } struct_;
        struct {
            char *case_name;
            EastValue *value;
            EastType *type;
        } variant;
        struct {
            EastValue *value;
        } ref;
        struct {
            void *data;   // float64*, int64_t*, or bool*
            size_t len;
            EastType *elem_type;
        } vector;
        struct {
            void *data;   // float64*, int64_t*, or bool*
            size_t rows;
            size_t cols;
            EastType *elem_type;
        } matrix;
        struct {
            EastCompiledFn *compiled;
        } function;
    } data;
};

// Global null singleton
extern EastValue east_null_value;

// Constructors
EastValue *east_null(void);
EastValue *east_boolean(bool val);
EastValue *east_integer(int64_t val);
EastValue *east_float(double val);
EastValue *east_string(const char *str);
EastValue *east_string_len(const char *str, size_t len);
EastValue *east_datetime(int64_t millis);
EastValue *east_blob(const uint8_t *data, size_t len);

// Collection constructors
EastValue *east_array_new(EastType *elem_type);
void east_array_push(EastValue *arr, EastValue *val);
EastValue *east_array_get(EastValue *arr, size_t index);
size_t east_array_len(EastValue *arr);

EastValue *east_set_new(EastType *elem_type);
void east_set_insert(EastValue *set, EastValue *val);
bool east_set_has(EastValue *set, EastValue *val);
bool east_set_delete(EastValue *set, EastValue *val);
size_t east_set_len(EastValue *set);

EastValue *east_dict_new(EastType *key_type, EastType *val_type);
void east_dict_set(EastValue *dict, EastValue *key, EastValue *val);
EastValue *east_dict_get(EastValue *dict, EastValue *key);
bool east_dict_has(EastValue *dict, EastValue *key);
bool east_dict_delete(EastValue *dict, EastValue *key);
EastValue *east_dict_pop(EastValue *dict, EastValue *key);
size_t east_dict_len(EastValue *dict);

EastValue *east_struct_new(const char **names, EastValue **values, size_t count, EastType *type);
EastValue *east_struct_get_field(EastValue *s, const char *name);

EastValue *east_variant_new(const char *case_name, EastValue *value, EastType *type);

EastValue *east_ref_new(EastValue *value);
EastValue *east_ref_get(EastValue *ref);
void east_ref_set(EastValue *ref, EastValue *value);

EastValue *east_vector_new(EastType *elem_type, size_t len);
EastValue *east_matrix_new(EastType *elem_type, size_t rows, size_t cols);

EastValue *east_function_value(EastCompiledFn *fn);

// Ref counting
void east_value_retain(EastValue *v);
void east_value_release(EastValue *v);

// Compiled function cleanup (defined in compiler.c)
void east_compiled_fn_free(EastCompiledFn *fn);

// Comparison
bool east_value_equal(EastValue *a, EastValue *b);
int east_value_compare(EastValue *a, EastValue *b);

// Printing
int east_value_print(EastValue *v, char *buf, size_t buf_size);

// Format a double using the shortest representation that round-trips,
// matching JavaScript's Number.toString() behavior.
int east_fmt_double(char *out, size_t out_size, double val);

// Type helpers
const char *east_value_kind_name(EastValueKind kind);

#endif
