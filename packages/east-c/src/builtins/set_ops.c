/*
 * Set builtin functions.
 */
#include "east/builtins.h"
#include "east/compiler.h"
#include "east/serialization.h"
#include "east/values.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Helper: infer type from value kind for printing */
static EastType *type_for_value(EastValue *v) {
    if (!v) return &east_null_type;
    switch (v->kind) {
    case EAST_VAL_INTEGER: return &east_integer_type;
    case EAST_VAL_FLOAT:   return &east_float_type;
    case EAST_VAL_BOOLEAN: return &east_boolean_type;
    case EAST_VAL_STRING:  return &east_string_type;
    default:               return &east_null_type;
    }
}

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

static EastValue *set_size_impl(EastValue **args, size_t n) {
    (void)n;
    return east_integer((int64_t)east_set_len(args[0]));
}

static EastValue *set_has_impl(EastValue **args, size_t n) {
    (void)n;
    return east_boolean(east_set_has(args[0], args[1]));
}

static EastValue *set_insert_impl(EastValue **args, size_t n) {
    (void)n;
    if (east_set_has(args[0], args[1])) {
        char *printed = east_print_value(args[1], type_for_value(args[1]));
        char msg[512];
        snprintf(msg, sizeof(msg), "Set already contains key %s",
                 printed ? printed : "?");
        free(printed);
        east_builtin_error(msg);
        return NULL;
    }
    east_set_insert(args[0], args[1]);
    return east_null();
}

static EastValue *set_try_insert_impl(EastValue **args, size_t n) {
    (void)n;
    bool was_new = !east_set_has(args[0], args[1]);
    east_set_insert(args[0], args[1]);
    return east_boolean(was_new);
}

static EastValue *set_delete_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *s = args[0];
    EastValue *val = args[1];
    if (east_set_delete(s, val))
        return east_null();
    char *printed = east_print_value(val, type_for_value(val));
    char msg[512];
    snprintf(msg, sizeof(msg), "Set does not contain key %s",
             printed ? printed : "?");
    free(printed);
    east_builtin_error(msg);
    return NULL;
}

static EastValue *set_try_delete_impl(EastValue **args, size_t n) {
    (void)n;
    return east_boolean(east_set_delete(args[0], args[1]));
}

static EastValue *set_clear_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *s = args[0];
    for (size_t i = 0; i < s->data.set.len; i++)
        east_value_release(s->data.set.items[i]);
    s->data.set.len = 0;
    return east_null();
}

static EastValue *set_union_in_place_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *a = args[0];
    EastValue *b = args[1];
    for (size_t i = 0; i < b->data.set.len; i++)
        east_set_insert(a, b->data.set.items[i]);
    return east_null();
}

static EastValue *set_union_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *a = args[0];
    EastValue *b = args[1];
    EastValue *result = east_set_new(a->data.set.elem_type);
    for (size_t i = 0; i < a->data.set.len; i++)
        east_set_insert(result, a->data.set.items[i]);
    for (size_t i = 0; i < b->data.set.len; i++)
        east_set_insert(result, b->data.set.items[i]);
    return result;
}

static EastValue *set_intersect_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *a = args[0];
    EastValue *b = args[1];
    EastValue *result = east_set_new(a->data.set.elem_type);
    for (size_t i = 0; i < a->data.set.len; i++) {
        if (east_set_has(b, a->data.set.items[i]))
            east_set_insert(result, a->data.set.items[i]);
    }
    return result;
}

static EastValue *set_diff_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *a = args[0];
    EastValue *b = args[1];
    EastValue *result = east_set_new(a->data.set.elem_type);
    for (size_t i = 0; i < a->data.set.len; i++) {
        if (!east_set_has(b, a->data.set.items[i]))
            east_set_insert(result, a->data.set.items[i]);
    }
    return result;
}

static EastValue *set_sym_diff_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *a = args[0];
    EastValue *b = args[1];
    EastValue *result = east_set_new(a->data.set.elem_type);
    for (size_t i = 0; i < a->data.set.len; i++) {
        if (!east_set_has(b, a->data.set.items[i]))
            east_set_insert(result, a->data.set.items[i]);
    }
    for (size_t i = 0; i < b->data.set.len; i++) {
        if (!east_set_has(a, b->data.set.items[i]))
            east_set_insert(result, b->data.set.items[i]);
    }
    return result;
}

static EastValue *set_is_subset_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *a = args[0];
    EastValue *b = args[1];
    for (size_t i = 0; i < a->data.set.len; i++) {
        if (!east_set_has(b, a->data.set.items[i]))
            return east_boolean(false);
    }
    return east_boolean(true);
}

static EastValue *set_is_disjoint_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *a = args[0];
    EastValue *b = args[1];
    for (size_t i = 0; i < a->data.set.len; i++) {
        if (east_set_has(b, a->data.set.items[i]))
            return east_boolean(false);
    }
    return east_boolean(true);
}

static EastValue *set_copy_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *s = args[0];
    EastValue *result = east_set_new(s->data.set.elem_type);
    for (size_t i = 0; i < s->data.set.len; i++)
        east_set_insert(result, s->data.set.items[i]);
    return result;
}

static EastValue *set_generate_impl(EastValue **args, size_t n) {
    (void)n;
    int64_t count = args[0]->data.integer;
    EastValue *gen_fn = args[1];
    EastValue *validate_fn = args[2];
    EastValue *result = east_set_new(&east_null_type);
    for (int64_t i = 0; i < count; i++) {
        EastValue *idx = east_integer(i);
        EastValue *call_args[] = { idx };
        EastValue *elem = call_fn(gen_fn, call_args, 1);
        if (east_set_has(result, elem)) {
            EastValue *vargs[] = { elem };
            EastValue *vr = call_fn(validate_fn, vargs, 1);
            east_value_release(vr);
        }
        east_set_insert(result, elem);
        east_value_release(elem);
        east_value_release(idx);
    }
    return result;
}

static EastValue *set_for_each_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *s = args[0];
    EastValue *fn = args[1];
    for (size_t i = 0; i < s->data.set.len; i++) {
        EastValue *call_args[] = { s->data.set.items[i] };
        EastValue *ret = call_fn(fn, call_args, 1);
        east_value_release(ret);
    }
    return east_null();
}

static EastValue *set_map_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *s = args[0];
    EastValue *fn = args[1];
    EastValue *result = east_dict_new(s->data.set.elem_type, &east_null_type);
    for (size_t i = 0; i < s->data.set.len; i++) {
        EastValue *call_args[] = { s->data.set.items[i] };
        EastValue *val = call_fn(fn, call_args, 1);
        east_dict_set(result, s->data.set.items[i], val);
        east_value_release(val);
    }
    return result;
}

static EastValue *set_filter_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *s = args[0];
    EastValue *fn = args[1];
    EastValue *result = east_set_new(s->data.set.elem_type);
    for (size_t i = 0; i < s->data.set.len; i++) {
        EastValue *call_args[] = { s->data.set.items[i] };
        EastValue *pred = call_fn(fn, call_args, 1);
        if (pred->data.boolean)
            east_set_insert(result, s->data.set.items[i]);
        east_value_release(pred);
    }
    return result;
}

static EastValue *set_filter_map_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *s = args[0];
    EastValue *fn = args[1];
    EastValue *result = east_dict_new(s->data.set.elem_type, &east_null_type);
    for (size_t i = 0; i < s->data.set.len; i++) {
        EastValue *call_args[] = { s->data.set.items[i] };
        EastValue *opt = call_fn(fn, call_args, 1);
        if (opt->kind == EAST_VAL_VARIANT && strcmp(opt->data.variant.case_name, "some") == 0)
            east_dict_set(result, s->data.set.items[i], opt->data.variant.value);
        east_value_release(opt);
    }
    return result;
}

static EastValue *set_first_map_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *s = args[0];
    EastValue *fn = args[1];
    for (size_t i = 0; i < s->data.set.len; i++) {
        EastValue *call_args[] = { s->data.set.items[i] };
        EastValue *opt = call_fn(fn, call_args, 1);
        if (opt->kind == EAST_VAL_VARIANT && strcmp(opt->data.variant.case_name, "some") == 0)
            return opt;
        east_value_release(opt);
    }
    return east_variant_new("none", east_null(), NULL);
}

static EastValue *set_map_reduce_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *s = args[0];
    EastValue *map_fn = args[1];
    EastValue *reduce_fn = args[2];
    if (s->data.set.len == 0) {
        east_builtin_error("Cannot reduce empty set with no initial value");
        return NULL;
    }
    EastValue *margs0[] = { s->data.set.items[0] };
    EastValue *acc = call_fn(map_fn, margs0, 1);
    for (size_t i = 1; i < s->data.set.len; i++) {
        EastValue *margs[] = { s->data.set.items[i] };
        EastValue *mapped = call_fn(map_fn, margs, 1);
        EastValue *rargs[] = { acc, mapped };
        EastValue *new_acc = call_fn(reduce_fn, rargs, 2);
        east_value_release(acc);
        east_value_release(mapped);
        acc = new_acc;
    }
    return acc;
}

static EastValue *set_reduce_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *s = args[0];
    EastValue *fn = args[1];
    EastValue *initial = args[2];
    east_value_retain(initial);
    EastValue *acc = initial;
    for (size_t i = 0; i < s->data.set.len; i++) {
        EastValue *call_args[] = { acc, s->data.set.items[i] };
        EastValue *new_acc = call_fn(fn, call_args, 2);
        east_value_release(acc);
        acc = new_acc;
    }
    return acc;
}

static EastValue *set_to_array_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *s = args[0];
    EastValue *fn = args[1];
    EastValue *result = east_array_new(s->data.set.elem_type);
    for (size_t i = 0; i < s->data.set.len; i++) {
        EastValue *call_args[] = { s->data.set.items[i] };
        EastValue *mapped = call_fn(fn, call_args, 1);
        east_array_push(result, mapped);
        east_value_release(mapped);
    }
    return result;
}

static EastValue *set_to_set_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *s = args[0];
    EastValue *fn = args[1];
    EastValue *result = east_set_new(&east_null_type);
    for (size_t i = 0; i < s->data.set.len; i++) {
        EastValue *call_args[] = { s->data.set.items[i] };
        EastValue *mapped = call_fn(fn, call_args, 1);
        east_set_insert(result, mapped);
        east_value_release(mapped);
    }
    return result;
}

static EastValue *set_to_dict_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *s = args[0];
    EastValue *key_fn = args[1];
    EastValue *value_fn = args[2];
    EastValue *merge_fn = args[3];
    EastValue *result = east_dict_new(&east_null_type, &east_null_type);
    for (size_t i = 0; i < s->data.set.len; i++) {
        EastValue *elem = s->data.set.items[i];
        EastValue *kargs[] = { elem };
        EastValue *key = call_fn(key_fn, kargs, 1);
        EastValue *vargs[] = { elem };
        EastValue *val = call_fn(value_fn, vargs, 1);
        if (east_dict_has(result, key)) {
            EastValue *existing = east_dict_get(result, key);
            EastValue *margs[] = { existing, val, key };
            EastValue *merged = call_fn(merge_fn, margs, 3);
            east_dict_set(result, key, merged);
            east_value_release(merged);
        } else {
            east_dict_set(result, key, val);
        }
        east_value_release(key);
        east_value_release(val);
    }
    return result;
}

static EastValue *set_flatten_to_array_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *s = args[0];
    EastValue *fn = args[1];
    EastValue *result = east_array_new(&east_null_type);
    for (size_t i = 0; i < s->data.set.len; i++) {
        EastValue *call_args[] = { s->data.set.items[i] };
        EastValue *mapped = call_fn(fn, call_args, 1);
        if (mapped->kind == EAST_VAL_ARRAY) {
            for (size_t j = 0; j < east_array_len(mapped); j++)
                east_array_push(result, east_array_get(mapped, j));
        }
        east_value_release(mapped);
    }
    return result;
}

static EastValue *set_flatten_to_set_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *s = args[0];
    EastValue *fn = args[1];
    EastValue *result = east_set_new(&east_null_type);
    for (size_t i = 0; i < s->data.set.len; i++) {
        EastValue *call_args[] = { s->data.set.items[i] };
        EastValue *mapped = call_fn(fn, call_args, 1);
        if (mapped->kind == EAST_VAL_SET) {
            for (size_t j = 0; j < mapped->data.set.len; j++)
                east_set_insert(result, mapped->data.set.items[j]);
        }
        east_value_release(mapped);
    }
    return result;
}

static EastValue *set_flatten_to_dict_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *s = args[0];
    EastValue *fn = args[1];
    EastValue *merge_fn = args[2];
    EastValue *result = east_dict_new(&east_null_type, &east_null_type);
    for (size_t i = 0; i < s->data.set.len; i++) {
        EastValue *call_args[] = { s->data.set.items[i] };
        EastValue *mapped = call_fn(fn, call_args, 1);
        if (mapped->kind == EAST_VAL_DICT) {
            for (size_t j = 0; j < mapped->data.dict.len; j++) {
                EastValue *k = mapped->data.dict.keys[j];
                EastValue *v = mapped->data.dict.values[j];
                if (east_dict_has(result, k)) {
                    EastValue *existing = east_dict_get(result, k);
                    EastValue *margs[] = { existing, v, k };
                    EastValue *merged = call_fn(merge_fn, margs, 3);
                    east_dict_set(result, k, merged);
                    east_value_release(merged);
                } else {
                    east_dict_set(result, k, v);
                }
            }
        }
        east_value_release(mapped);
    }
    return result;
}

static EastValue *set_group_fold_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *s = args[0];
    EastValue *key_fn = args[1];
    EastValue *init_fn = args[2];
    EastValue *fold_fn = args[3];
    EastValue *result = east_dict_new(&east_null_type, &east_null_type);
    for (size_t i = 0; i < s->data.set.len; i++) {
        EastValue *elem = s->data.set.items[i];
        EastValue *kargs[] = { elem };
        EastValue *key = call_fn(key_fn, kargs, 1);
        if (!east_dict_has(result, key)) {
            EastValue *iargs[] = { key };
            EastValue *init = call_fn(init_fn, iargs, 1);
            east_dict_set(result, key, init);
            east_value_release(init);
        }
        EastValue *acc = east_dict_get(result, key);
        EastValue *fargs[] = { acc, elem };
        EastValue *new_acc = call_fn(fold_fn, fargs, 2);
        east_dict_set(result, key, new_acc);
        east_value_release(new_acc);
        east_value_release(key);
    }
    return result;
}

/* --- factory functions --- */

static BuiltinImpl set_generate_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_generate_impl; }
static BuiltinImpl set_size_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_size_impl; }
static BuiltinImpl set_has_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_has_impl; }
static BuiltinImpl set_insert_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_insert_impl; }
static BuiltinImpl set_try_insert_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_try_insert_impl; }
static BuiltinImpl set_delete_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_delete_impl; }
static BuiltinImpl set_try_delete_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_try_delete_impl; }
static BuiltinImpl set_clear_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_clear_impl; }
static BuiltinImpl set_union_in_place_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_union_in_place_impl; }
static BuiltinImpl set_union_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_union_impl; }
static BuiltinImpl set_intersect_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_intersect_impl; }
static BuiltinImpl set_diff_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_diff_impl; }
static BuiltinImpl set_sym_diff_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_sym_diff_impl; }
static BuiltinImpl set_is_subset_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_is_subset_impl; }
static BuiltinImpl set_is_disjoint_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_is_disjoint_impl; }
static BuiltinImpl set_copy_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_copy_impl; }
static BuiltinImpl set_for_each_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_for_each_impl; }
static BuiltinImpl set_map_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_map_impl; }
static BuiltinImpl set_filter_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_filter_impl; }
static BuiltinImpl set_filter_map_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_filter_map_impl; }
static BuiltinImpl set_first_map_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_first_map_impl; }
static BuiltinImpl set_map_reduce_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_map_reduce_impl; }
static BuiltinImpl set_reduce_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_reduce_impl; }
static BuiltinImpl set_to_array_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_to_array_impl; }
static BuiltinImpl set_to_set_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_to_set_impl; }
static BuiltinImpl set_to_dict_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_to_dict_impl; }
static BuiltinImpl set_flatten_to_array_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_flatten_to_array_impl; }
static BuiltinImpl set_flatten_to_set_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_flatten_to_set_impl; }
static BuiltinImpl set_flatten_to_dict_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_flatten_to_dict_impl; }
static BuiltinImpl set_group_fold_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return set_group_fold_impl; }

/* --- registration --- */

void east_register_set_builtins(BuiltinRegistry *reg) {
    builtin_registry_register(reg, "SetGenerate", set_generate_factory);
    builtin_registry_register(reg, "SetSize", set_size_factory);
    builtin_registry_register(reg, "SetHas", set_has_factory);
    builtin_registry_register(reg, "SetInsert", set_insert_factory);
    builtin_registry_register(reg, "SetTryInsert", set_try_insert_factory);
    builtin_registry_register(reg, "SetDelete", set_delete_factory);
    builtin_registry_register(reg, "SetTryDelete", set_try_delete_factory);
    builtin_registry_register(reg, "SetClear", set_clear_factory);
    builtin_registry_register(reg, "SetUnionInPlace", set_union_in_place_factory);
    builtin_registry_register(reg, "SetUnion", set_union_factory);
    builtin_registry_register(reg, "SetIntersect", set_intersect_factory);
    builtin_registry_register(reg, "SetDiff", set_diff_factory);
    builtin_registry_register(reg, "SetSymDiff", set_sym_diff_factory);
    builtin_registry_register(reg, "SetIsSubset", set_is_subset_factory);
    builtin_registry_register(reg, "SetIsDisjoint", set_is_disjoint_factory);
    builtin_registry_register(reg, "SetCopy", set_copy_factory);
    builtin_registry_register(reg, "SetForEach", set_for_each_factory);
    builtin_registry_register(reg, "SetMap", set_map_factory);
    builtin_registry_register(reg, "SetFilter", set_filter_factory);
    builtin_registry_register(reg, "SetFilterMap", set_filter_map_factory);
    builtin_registry_register(reg, "SetFirstMap", set_first_map_factory);
    builtin_registry_register(reg, "SetMapReduce", set_map_reduce_factory);
    builtin_registry_register(reg, "SetReduce", set_reduce_factory);
    builtin_registry_register(reg, "SetToArray", set_to_array_factory);
    builtin_registry_register(reg, "SetToSet", set_to_set_factory);
    builtin_registry_register(reg, "SetToDict", set_to_dict_factory);
    builtin_registry_register(reg, "SetFlattenToArray", set_flatten_to_array_factory);
    builtin_registry_register(reg, "SetFlattenToSet", set_flatten_to_set_factory);
    builtin_registry_register(reg, "SetFlattenToDict", set_flatten_to_dict_factory);
    builtin_registry_register(reg, "SetGroupFold", set_group_fold_factory);
}
