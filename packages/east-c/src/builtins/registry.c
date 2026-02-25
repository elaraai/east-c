/*
 * Builtin registry implementation.
 * Maps builtin names to factory functions.
 */
#include "east/builtins.h"
#include <stdlib.h>
#include <string.h>

/* Global builtin error string (set by builtins, read by compiler) */
static _Thread_local char *g_builtin_error = NULL;

void east_builtin_error(const char *msg) {
    free(g_builtin_error);
    g_builtin_error = msg ? strdup(msg) : NULL;
}

char *east_builtin_get_error(void) {
    char *err = g_builtin_error;
    g_builtin_error = NULL;
    return err; /* caller owns */
}

BuiltinRegistry *builtin_registry_new(void) {
    BuiltinRegistry *reg = malloc(sizeof(BuiltinRegistry));
    if (!reg) return NULL;
    reg->factories = hashmap_new();
    return reg;
}

void builtin_registry_register(BuiltinRegistry *reg, const char *name, BuiltinFactory factory) {
    /* Store factory function pointer as void* in the hashmap */
    hashmap_set(reg->factories, name, (void *)(uintptr_t)factory);
}

BuiltinImpl builtin_registry_get(BuiltinRegistry *reg, const char *name, EastType **type_params, size_t num_tp) {
    void *raw = hashmap_get(reg->factories, name);
    if (!raw) return NULL;
    BuiltinFactory factory = (BuiltinFactory)(uintptr_t)raw;
    return factory(type_params, num_tp);
}

void builtin_registry_free(BuiltinRegistry *reg) {
    if (!reg) return;
    /* Factory function pointers are not heap-allocated, so pass NULL free */
    hashmap_free(reg->factories, NULL);
    free(reg);
}

void east_register_all_builtins(BuiltinRegistry *reg) {
    east_register_integer_builtins(reg);
    east_register_float_builtins(reg);
    east_register_boolean_builtins(reg);
    east_register_string_builtins(reg);
    east_register_comparison_builtins(reg);
    east_register_datetime_builtins(reg);
    east_register_blob_builtins(reg);
    east_register_array_builtins(reg);
    east_register_set_builtins(reg);
    east_register_dict_builtins(reg);
    east_register_ref_builtins(reg);
    east_register_vector_builtins(reg);
    east_register_matrix_builtins(reg);
    east_register_patch_builtins(reg);
}
