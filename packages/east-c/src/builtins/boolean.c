/*
 * Boolean builtin functions.
 */
#include "east/builtins.h"
#include "east/values.h"
#include <stdlib.h>

/* --- static implementations --- */

static EastValue *boolean_and(EastValue **args, size_t n) {
    (void)n;
    return east_boolean(args[0]->data.boolean && args[1]->data.boolean);
}

static EastValue *boolean_or(EastValue **args, size_t n) {
    (void)n;
    return east_boolean(args[0]->data.boolean || args[1]->data.boolean);
}

static EastValue *boolean_not(EastValue **args, size_t n) {
    (void)n;
    return east_boolean(!args[0]->data.boolean);
}

static EastValue *boolean_xor(EastValue **args, size_t n) {
    (void)n;
    return east_boolean(args[0]->data.boolean != args[1]->data.boolean);
}

/* --- factory functions --- */

static BuiltinImpl boolean_and_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return boolean_and; }
static BuiltinImpl boolean_or_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return boolean_or; }
static BuiltinImpl boolean_not_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return boolean_not; }
static BuiltinImpl boolean_xor_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return boolean_xor; }

/* --- registration --- */

void east_register_boolean_builtins(BuiltinRegistry *reg) {
    builtin_registry_register(reg, "BooleanAnd", boolean_and_factory);
    builtin_registry_register(reg, "BooleanOr", boolean_or_factory);
    builtin_registry_register(reg, "BooleanNot", boolean_not_factory);
    builtin_registry_register(reg, "BooleanXor", boolean_xor_factory);
}
