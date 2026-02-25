#ifndef EAST_TYPES_H
#define EAST_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    EAST_TYPE_NEVER,
    EAST_TYPE_NULL,
    EAST_TYPE_BOOLEAN,
    EAST_TYPE_INTEGER,
    EAST_TYPE_FLOAT,
    EAST_TYPE_STRING,
    EAST_TYPE_DATETIME,
    EAST_TYPE_BLOB,
    EAST_TYPE_ARRAY,
    EAST_TYPE_SET,
    EAST_TYPE_DICT,
    EAST_TYPE_STRUCT,
    EAST_TYPE_VARIANT,
    EAST_TYPE_REF,
    EAST_TYPE_VECTOR,
    EAST_TYPE_MATRIX,
    EAST_TYPE_FUNCTION,
    EAST_TYPE_ASYNC_FUNCTION,
    EAST_TYPE_RECURSIVE,
} EastTypeKind;

typedef struct EastType EastType;

typedef struct {
    char *name;
    EastType *type;
} EastTypeField;

struct EastType {
    EastTypeKind kind;
    int ref_count;
    union {
        // Array, Set, Ref, Vector, Matrix: element type
        EastType *element;
        // Dict: key and value types
        struct {
            EastType *key;
            EastType *value;
        } dict;
        // Struct: named fields
        struct {
            EastTypeField *fields;
            size_t num_fields;
        } struct_;
        // Variant: named cases
        struct {
            EastTypeField *cases;
            size_t num_cases;
        } variant;
        // Function, AsyncFunction: inputs and output
        struct {
            EastType **inputs;
            size_t num_inputs;
            EastType *output;
        } function;
        // Recursive: wrapper around inner type (node).
        // Self-references inside node point back to this wrapper.
        // Matches TypeScript RecursiveType({ type: "Recursive", node: ... })
        struct {
            EastType *node;
            int internal_refs;  // number of back-references from inner tree
        } recursive;
    } data;
};

// Primitive type singletons (never freed)
extern EastType east_never_type;
extern EastType east_null_type;
extern EastType east_boolean_type;
extern EastType east_integer_type;
extern EastType east_float_type;
extern EastType east_string_type;
extern EastType east_datetime_type;
extern EastType east_blob_type;

// Constructors (return ref_count=1)
EastType *east_array_type(EastType *elem);
EastType *east_set_type(EastType *elem);
EastType *east_dict_type(EastType *key, EastType *val);
EastType *east_struct_type(const char **names, EastType **types, size_t count);
EastType *east_variant_type(const char **names, EastType **types, size_t count);
EastType *east_ref_type(EastType *inner);
EastType *east_vector_type(EastType *elem);
EastType *east_matrix_type(EastType *elem);
EastType *east_function_type(EastType **inputs, size_t num_inputs, EastType *output);
EastType *east_async_function_type(EastType **inputs, size_t num_inputs, EastType *output);

// Recursive type: wrapper with inner node.
// Usage: create wrapper first, then build inner type using wrapper as self-ref,
// then call east_recursive_type_set to close the cycle, then call
// east_recursive_type_finalize to adjust refcounts for cycle-aware cleanup.
EastType *east_recursive_type_new(void);
void east_recursive_type_set(EastType *rec, EastType *node);
// Must be called after east_recursive_type_set to enable automatic cycle breaking.
// Counts internal back-references and adjusts refcount so only external refs are tracked.
void east_recursive_type_finalize(EastType *rec);

// Ref counting
void east_type_retain(EastType *t);
void east_type_release(EastType *t);

// Comparison
bool east_type_equal(EastType *a, EastType *b);

// Printing (writes to buffer, returns chars written)
int east_type_print(EastType *t, char *buf, size_t buf_size);

// Helpers
const char *east_type_kind_name(EastTypeKind kind);

#endif
