#ifndef EAST_BUILTINS_H
#define EAST_BUILTINS_H

#include "types.h"
#include "values.h"
#include "hashmap.h"
#include <stddef.h>

// A builtin implementation takes args and returns a value.
// To signal an error: call east_builtin_error("msg") and return NULL.
typedef EastValue *(*BuiltinImpl)(EastValue **args, size_t num_args);

// Set a builtin error message. The caller should then return NULL.
void east_builtin_error(const char *msg);
// Get and clear the last builtin error. Returns NULL if no error.
// Caller takes ownership of the returned string (must free).
char *east_builtin_get_error(void);

// A builtin factory takes type parameters and returns an implementation
typedef BuiltinImpl (*BuiltinFactory)(EastType **type_params, size_t num_type_params);

typedef struct {
    Hashmap *factories;  // name -> BuiltinFactory (cast via void*)
} BuiltinRegistry;

BuiltinRegistry *builtin_registry_new(void);
void builtin_registry_register(BuiltinRegistry *reg, const char *name, BuiltinFactory factory);
BuiltinImpl builtin_registry_get(BuiltinRegistry *reg, const char *name, EastType **type_params, size_t num_tp);
void builtin_registry_free(BuiltinRegistry *reg);

// Register all builtins
void east_register_all_builtins(BuiltinRegistry *reg);

// Individual module registration
void east_register_integer_builtins(BuiltinRegistry *reg);
void east_register_float_builtins(BuiltinRegistry *reg);
void east_register_boolean_builtins(BuiltinRegistry *reg);
void east_register_string_builtins(BuiltinRegistry *reg);
void east_register_comparison_builtins(BuiltinRegistry *reg);
void east_register_datetime_builtins(BuiltinRegistry *reg);
void east_register_blob_builtins(BuiltinRegistry *reg);
void east_register_array_builtins(BuiltinRegistry *reg);
void east_register_set_builtins(BuiltinRegistry *reg);
void east_register_dict_builtins(BuiltinRegistry *reg);
void east_register_ref_builtins(BuiltinRegistry *reg);
void east_register_vector_builtins(BuiltinRegistry *reg);
void east_register_matrix_builtins(BuiltinRegistry *reg);
void east_register_patch_builtins(BuiltinRegistry *reg);

#endif
