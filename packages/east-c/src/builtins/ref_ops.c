/*
 * Ref builtin functions.
 */
#include "east/builtins.h"
#include "east/compiler.h"
#include "east/values.h"
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Helper: call a function value                                      */
/* ------------------------------------------------------------------ */
static EastValue *call_fn(EastValue *fn, EastValue **call_args, size_t nargs) {
    EvalResult r = east_call(fn->data.function.compiled, call_args, nargs);
    if (r.status == EVAL_OK || r.status == EVAL_RETURN) return r.value;
    eval_result_free(&r);
    return east_null();
}

/* --- implementations --- */

static EastValue *ref_get_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *v = east_ref_get(args[0]);
    if (v) east_value_retain(v);
    return v;
}

static EastValue *ref_update_impl(EastValue **args, size_t n) {
    (void)n;
    east_ref_set(args[0], args[1]);
    return east_null();
}

static EastValue *ref_merge_impl(EastValue **args, size_t n) {
    (void)n;
    /* args: ref, new_value, update_fn */
    EastValue *current = east_ref_get(args[0]);
    EastValue *call_args[] = { current, args[1] };
    EastValue *merged = call_fn(args[2], call_args, 2);
    east_ref_set(args[0], merged);
    east_value_release(merged);
    return east_null();
}

/* --- factory functions --- */

static BuiltinImpl ref_get_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return ref_get_impl; }
static BuiltinImpl ref_update_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return ref_update_impl; }
static BuiltinImpl ref_merge_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return ref_merge_impl; }

/* --- registration --- */

void east_register_ref_builtins(BuiltinRegistry *reg) {
    builtin_registry_register(reg, "RefGet", ref_get_factory);
    builtin_registry_register(reg, "RefUpdate", ref_update_factory);
    builtin_registry_register(reg, "RefMerge", ref_merge_factory);
}
