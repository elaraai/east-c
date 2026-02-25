/*
 * Dict builtin functions.
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

/* Helper: format a "Dict does not contain key <key>" error message */
static void dict_key_not_found_error(EastValue *key) {
    char *printed = east_print_value(key, type_for_value(key));
    char msg[512];
    snprintf(msg, sizeof(msg), "Dict does not contain key %s",
             printed ? printed : "?");
    free(printed);
    east_builtin_error(msg);
}

/* Helper: format a "Dict already contains key <key>" error message */
static void dict_key_already_exists_error(EastValue *key) {
    char *printed = east_print_value(key, type_for_value(key));
    char msg[512];
    snprintf(msg, sizeof(msg), "Dict already contains key %s",
             printed ? printed : "?");
    free(printed);
    east_builtin_error(msg);
}

/* ------------------------------------------------------------------ */
/* Helper: call a function value                                      */
/* ------------------------------------------------------------------ */
static EastValue *call_fn(EastValue *fn, EastValue **call_args, size_t nargs) {
    EvalResult r = east_call(fn->data.function.compiled, call_args, nargs);
    if (r.status == EVAL_OK || r.status == EVAL_RETURN) return r.value;
    /* Propagate error from callback */
    if (r.error_message) {
        east_builtin_error(r.error_message);
    }
    eval_result_free(&r);
    return NULL;
}

/* --- implementations --- */

static EastValue *dict_size_impl(EastValue **args, size_t n) {
    (void)n;
    return east_integer((int64_t)east_dict_len(args[0]));
}

static EastValue *dict_has_impl(EastValue **args, size_t n) {
    (void)n;
    return east_boolean(east_dict_has(args[0], args[1]));
}

static EastValue *dict_get_impl(EastValue **args, size_t n) {
    (void)n;
    if (!east_dict_has(args[0], args[1])) {
        dict_key_not_found_error(args[1]);
        return NULL;
    }
    EastValue *v = east_dict_get(args[0], args[1]);
    if (v) east_value_retain(v);
    return v;
}

static EastValue *dict_get_or_default_impl(EastValue **args, size_t n) {
    (void)n;
    if (east_dict_has(args[0], args[1])) {
        EastValue *v = east_dict_get(args[0], args[1]);
        if (v) east_value_retain(v);
        return v;
    }
    EastValue *call_args[] = { args[1] };
    return call_fn(args[2], call_args, 1);
}

static EastValue *dict_try_get_impl(EastValue **args, size_t n) {
    (void)n;
    if (east_dict_has(args[0], args[1])) {
        EastValue *val = east_dict_get(args[0], args[1]);
        return east_variant_new("some", val, NULL);
    }
    return east_variant_new("none", east_null(), NULL);
}

static EastValue *dict_insert_impl(EastValue **args, size_t n) {
    (void)n;
    if (east_dict_has(args[0], args[1])) {
        dict_key_already_exists_error(args[1]);
        return NULL;
    }
    east_dict_set(args[0], args[1], args[2]);
    return east_null();
}

static EastValue *dict_get_or_insert_impl(EastValue **args, size_t n) {
    (void)n;
    if (east_dict_has(args[0], args[1])) {
        EastValue *v = east_dict_get(args[0], args[1]);
        if (v) east_value_retain(v);
        return v;
    }
    EastValue *call_args[] = { args[1] };
    EastValue *val = call_fn(args[2], call_args, 1);
    east_dict_set(args[0], args[1], val);
    return val;
}

static EastValue *dict_insert_or_update_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *key = args[1];
    EastValue *value = args[2];
    EastValue *merge_fn = args[3];
    if (east_dict_has(d, key)) {
        EastValue *existing = east_dict_get(d, key);
        EastValue *margs[] = { existing, value, key };
        EastValue *merged = call_fn(merge_fn, margs, 3);
        east_dict_set(d, key, merged);
        east_value_release(merged);
    } else {
        east_dict_set(d, key, value);
    }
    return east_null();
}

static EastValue *dict_update_impl(EastValue **args, size_t n) {
    (void)n;
    if (!east_dict_has(args[0], args[1])) {
        dict_key_not_found_error(args[1]);
        return NULL;
    }
    east_dict_set(args[0], args[1], args[2]);
    return east_null();
}

static EastValue *dict_swap_impl(EastValue **args, size_t n) {
    (void)n;
    if (!east_dict_has(args[0], args[1])) {
        dict_key_not_found_error(args[1]);
        return NULL;
    }
    EastValue *old = east_dict_get(args[0], args[1]);
    east_value_retain(old);
    east_dict_set(args[0], args[1], args[2]);
    return old;
}

static EastValue *dict_merge_impl(EastValue **args, size_t n) {
    (void)n;
    /* args: d, key, value, merge_fn, initial_fn */
    EastValue *d = args[0];
    EastValue *key = args[1];
    EastValue *value = args[2];
    EastValue *merge_fn = args[3];
    EastValue *initial_fn = args[4];
    EastValue *existing;
    bool existing_owned = false;
    if (east_dict_has(d, key)) {
        existing = east_dict_get(d, key);
    } else {
        EastValue *iargs[] = { key };
        existing = call_fn(initial_fn, iargs, 1);
        if (!existing) return NULL;  /* initial_fn threw */
        existing_owned = true;
    }
    EastValue *margs[] = { existing, value, key };
    EastValue *merged = call_fn(merge_fn, margs, 3);
    if (existing_owned) east_value_release(existing);
    if (!merged) return NULL;  /* merge_fn threw */
    east_dict_set(d, key, merged);
    east_value_release(merged);
    return east_null();
}

static EastValue *dict_delete_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *key = args[1];
    if (east_dict_delete(d, key))
        return east_null();
    dict_key_not_found_error(key);
    return NULL;
}

static EastValue *dict_try_delete_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *key = args[1];
    return east_boolean(east_dict_delete(d, key));
}

static EastValue *dict_pop_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *key = args[1];
    EastValue *val = east_dict_pop(d, key);
    if (val) return val;
    dict_key_not_found_error(key);
    return NULL;
}

static EastValue *dict_clear_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    for (size_t i = 0; i < d->data.dict.len; i++) {
        east_value_release(d->data.dict.keys[i]);
        east_value_release(d->data.dict.values[i]);
    }
    d->data.dict.len = 0;
    return east_null();
}

static EastValue *dict_union_in_place_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *other = args[1];
    EastValue *merge_fn = args[2];
    for (size_t i = 0; i < other->data.dict.len; i++) {
        EastValue *k = other->data.dict.keys[i];
        EastValue *v = other->data.dict.values[i];
        if (east_dict_has(d, k)) {
            EastValue *existing = east_dict_get(d, k);
            EastValue *margs[] = { existing, v, k };
            EastValue *merged = call_fn(merge_fn, margs, 3);
            if (!merged) return NULL;  /* merge_fn threw */
            east_dict_set(d, k, merged);
            east_value_release(merged);
        } else {
            east_dict_set(d, k, v);
        }
    }
    return east_null();
}

static EastValue *dict_merge_all_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *other = args[1];
    EastValue *merge_fn = args[2];
    EastValue *default_fn = args[3];
    for (size_t i = 0; i < other->data.dict.len; i++) {
        EastValue *k = other->data.dict.keys[i];
        EastValue *v = other->data.dict.values[i];
        EastValue *existing;
        bool existing_owned = false;
        if (east_dict_has(d, k)) {
            existing = east_dict_get(d, k);
        } else {
            EastValue *dargs[] = { k };
            existing = call_fn(default_fn, dargs, 1);
            if (!existing) return NULL;  /* default_fn threw */
            existing_owned = true;
        }
        EastValue *margs[] = { existing, v, k };
        EastValue *merged = call_fn(merge_fn, margs, 3);
        if (existing_owned) east_value_release(existing);
        if (!merged) return NULL;  /* merge_fn threw */
        east_dict_set(d, k, merged);
        east_value_release(merged);
    }
    return east_null();
}

static EastValue *dict_keys_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *result = east_set_new(d->data.dict.key_type);
    for (size_t i = 0; i < d->data.dict.len; i++)
        east_set_insert(result, d->data.dict.keys[i]);
    return result;
}

static EastValue *dict_get_keys_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *keys_set = args[1];
    EastValue *default_fn = args[2];
    EastValue *result = east_dict_new(d->data.dict.key_type, d->data.dict.val_type);
    for (size_t i = 0; i < keys_set->data.set.len; i++) {
        EastValue *k = keys_set->data.set.items[i];
        if (east_dict_has(d, k)) {
            east_dict_set(result, k, east_dict_get(d, k));
        } else {
            EastValue *dargs[] = { k };
            EastValue *def = call_fn(default_fn, dargs, 1);
            east_dict_set(result, k, def);
            east_value_release(def);
        }
    }
    return result;
}

static EastValue *dict_generate_impl(EastValue **args, size_t n) {
    (void)n;
    int64_t count = args[0]->data.integer;
    EastValue *key_fn = args[1];
    EastValue *value_fn = args[2];
    EastValue *merge_fn = args[3];
    EastValue *result = east_dict_new(&east_null_type, &east_null_type);
    for (int64_t i = 0; i < count; i++) {
        EastValue *idx = east_integer(i);
        EastValue *kargs[] = { idx };
        EastValue *key = call_fn(key_fn, kargs, 1);
        EastValue *vargs[] = { idx };
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
        east_value_release(idx);
    }
    return result;
}

static EastValue *dict_copy_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *result = east_dict_new(d->data.dict.key_type, d->data.dict.val_type);
    for (size_t i = 0; i < d->data.dict.len; i++)
        east_dict_set(result, d->data.dict.keys[i], d->data.dict.values[i]);
    return result;
}

static EastValue *dict_for_each_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *fn = args[1];
    for (size_t i = 0; i < d->data.dict.len; i++) {
        EastValue *call_args[] = { d->data.dict.values[i], d->data.dict.keys[i] };
        EastValue *ret = call_fn(fn, call_args, 2);
        east_value_release(ret);
    }
    return east_null();
}

static EastValue *dict_map_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *fn = args[1];
    EastValue *result = east_dict_new(d->data.dict.key_type, &east_null_type);
    for (size_t i = 0; i < d->data.dict.len; i++) {
        EastValue *call_args[] = { d->data.dict.values[i], d->data.dict.keys[i] };
        EastValue *val = call_fn(fn, call_args, 2);
        east_dict_set(result, d->data.dict.keys[i], val);
        east_value_release(val);
    }
    return result;
}

static EastValue *dict_filter_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *fn = args[1];
    EastValue *result = east_dict_new(d->data.dict.key_type, d->data.dict.val_type);
    for (size_t i = 0; i < d->data.dict.len; i++) {
        EastValue *call_args[] = { d->data.dict.values[i], d->data.dict.keys[i] };
        EastValue *pred = call_fn(fn, call_args, 2);
        if (pred->data.boolean)
            east_dict_set(result, d->data.dict.keys[i], d->data.dict.values[i]);
        east_value_release(pred);
    }
    return result;
}

static EastValue *dict_filter_map_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *fn = args[1];
    EastValue *result = east_dict_new(d->data.dict.key_type, &east_null_type);
    for (size_t i = 0; i < d->data.dict.len; i++) {
        EastValue *call_args[] = { d->data.dict.values[i], d->data.dict.keys[i] };
        EastValue *opt = call_fn(fn, call_args, 2);
        if (opt->kind == EAST_VAL_VARIANT && strcmp(opt->data.variant.case_name, "some") == 0)
            east_dict_set(result, d->data.dict.keys[i], opt->data.variant.value);
        east_value_release(opt);
    }
    return result;
}

static EastValue *dict_first_map_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *fn = args[1];
    for (size_t i = 0; i < d->data.dict.len; i++) {
        EastValue *call_args[] = { d->data.dict.values[i], d->data.dict.keys[i] };
        EastValue *opt = call_fn(fn, call_args, 2);
        if (opt->kind == EAST_VAL_VARIANT && strcmp(opt->data.variant.case_name, "some") == 0)
            return opt;
        east_value_release(opt);
    }
    return east_variant_new("none", east_null(), NULL);
}

static EastValue *dict_map_reduce_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *map_fn = args[1];
    EastValue *reduce_fn = args[2];
    if (d->data.dict.len == 0) {
        east_builtin_error("Cannot reduce empty dictionary with no initial value");
        return NULL;
    }
    EastValue *margs0[] = { d->data.dict.values[0], d->data.dict.keys[0] };
    EastValue *acc = call_fn(map_fn, margs0, 2);
    for (size_t i = 1; i < d->data.dict.len; i++) {
        EastValue *margs[] = { d->data.dict.values[i], d->data.dict.keys[i] };
        EastValue *mapped = call_fn(map_fn, margs, 2);
        EastValue *rargs[] = { acc, mapped };
        EastValue *new_acc = call_fn(reduce_fn, rargs, 2);
        east_value_release(acc);
        east_value_release(mapped);
        acc = new_acc;
    }
    return acc;
}

static EastValue *dict_reduce_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *fn = args[1];
    EastValue *initial = args[2];
    east_value_retain(initial);
    EastValue *acc = initial;
    for (size_t i = 0; i < d->data.dict.len; i++) {
        EastValue *call_args[] = { acc, d->data.dict.values[i], d->data.dict.keys[i] };
        EastValue *new_acc = call_fn(fn, call_args, 3);
        east_value_release(acc);
        acc = new_acc;
    }
    return acc;
}

static EastValue *dict_to_array_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *fn = args[1];
    EastValue *result = east_array_new(&east_null_type);
    for (size_t i = 0; i < d->data.dict.len; i++) {
        EastValue *call_args[] = { d->data.dict.values[i], d->data.dict.keys[i] };
        EastValue *val = call_fn(fn, call_args, 2);
        east_array_push(result, val);
        east_value_release(val);
    }
    return result;
}

static EastValue *dict_to_set_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *fn = args[1];
    EastValue *result = east_set_new(&east_null_type);
    for (size_t i = 0; i < d->data.dict.len; i++) {
        EastValue *call_args[] = { d->data.dict.values[i], d->data.dict.keys[i] };
        EastValue *val = call_fn(fn, call_args, 2);
        east_set_insert(result, val);
        east_value_release(val);
    }
    return result;
}

static EastValue *dict_to_dict_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *key_fn = args[1];
    EastValue *value_fn = args[2];
    EastValue *merge_fn = args[3];
    EastValue *result = east_dict_new(&east_null_type, &east_null_type);
    for (size_t i = 0; i < d->data.dict.len; i++) {
        EastValue *v = d->data.dict.values[i];
        EastValue *k = d->data.dict.keys[i];
        EastValue *kargs[] = { v, k };
        EastValue *new_key = call_fn(key_fn, kargs, 2);
        EastValue *vargs[] = { v, k };
        EastValue *new_val = call_fn(value_fn, vargs, 2);
        if (east_dict_has(result, new_key)) {
            EastValue *existing = east_dict_get(result, new_key);
            EastValue *margs[] = { existing, new_val, new_key };
            EastValue *merged = call_fn(merge_fn, margs, 3);
            east_dict_set(result, new_key, merged);
            east_value_release(merged);
        } else {
            east_dict_set(result, new_key, new_val);
        }
        east_value_release(new_key);
        east_value_release(new_val);
    }
    return result;
}

static EastValue *dict_flatten_to_array_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *fn = args[1];
    EastValue *result = east_array_new(&east_null_type);
    for (size_t i = 0; i < d->data.dict.len; i++) {
        EastValue *call_args[] = { d->data.dict.values[i], d->data.dict.keys[i] };
        EastValue *mapped = call_fn(fn, call_args, 2);
        if (mapped->kind == EAST_VAL_ARRAY) {
            for (size_t j = 0; j < east_array_len(mapped); j++)
                east_array_push(result, east_array_get(mapped, j));
        }
        east_value_release(mapped);
    }
    return result;
}

static EastValue *dict_flatten_to_set_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *fn = args[1];
    EastValue *result = east_set_new(&east_null_type);
    for (size_t i = 0; i < d->data.dict.len; i++) {
        EastValue *call_args[] = { d->data.dict.values[i], d->data.dict.keys[i] };
        EastValue *mapped = call_fn(fn, call_args, 2);
        if (mapped->kind == EAST_VAL_SET) {
            for (size_t j = 0; j < mapped->data.set.len; j++)
                east_set_insert(result, mapped->data.set.items[j]);
        }
        east_value_release(mapped);
    }
    return result;
}

static EastValue *dict_flatten_to_dict_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *fn = args[1];
    EastValue *merge_fn = args[2];
    EastValue *result = east_dict_new(&east_null_type, &east_null_type);
    for (size_t i = 0; i < d->data.dict.len; i++) {
        EastValue *call_args[] = { d->data.dict.values[i], d->data.dict.keys[i] };
        EastValue *mapped = call_fn(fn, call_args, 2);
        if (mapped->kind == EAST_VAL_DICT) {
            for (size_t j = 0; j < mapped->data.dict.len; j++) {
                EastValue *mk = mapped->data.dict.keys[j];
                EastValue *mv = mapped->data.dict.values[j];
                if (east_dict_has(result, mk)) {
                    EastValue *existing = east_dict_get(result, mk);
                    EastValue *margs[] = { existing, mv, mk };
                    EastValue *merged = call_fn(merge_fn, margs, 3);
                    east_dict_set(result, mk, merged);
                    east_value_release(merged);
                } else {
                    east_dict_set(result, mk, mv);
                }
            }
        }
        east_value_release(mapped);
    }
    return result;
}

static EastValue *dict_group_fold_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *d = args[0];
    EastValue *key_fn = args[1];
    EastValue *init_fn = args[2];
    EastValue *fold_fn = args[3];
    EastValue *result = east_dict_new(&east_null_type, &east_null_type);
    for (size_t i = 0; i < d->data.dict.len; i++) {
        EastValue *v = d->data.dict.values[i];
        EastValue *k = d->data.dict.keys[i];
        EastValue *kargs[] = { v, k };
        EastValue *group_key = call_fn(key_fn, kargs, 2);
        if (!east_dict_has(result, group_key)) {
            EastValue *iargs[] = { group_key };
            EastValue *init = call_fn(init_fn, iargs, 1);
            east_dict_set(result, group_key, init);
            east_value_release(init);
        }
        EastValue *acc = east_dict_get(result, group_key);
        EastValue *fargs[] = { acc, v, k };
        EastValue *new_acc = call_fn(fold_fn, fargs, 3);
        east_dict_set(result, group_key, new_acc);
        east_value_release(new_acc);
        east_value_release(group_key);
    }
    return result;
}

/* --- factory functions --- */

static BuiltinImpl dict_generate_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_generate_impl; }
static BuiltinImpl dict_size_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_size_impl; }
static BuiltinImpl dict_has_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_has_impl; }
static BuiltinImpl dict_get_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_get_impl; }
static BuiltinImpl dict_get_or_default_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_get_or_default_impl; }
static BuiltinImpl dict_try_get_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_try_get_impl; }
static BuiltinImpl dict_insert_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_insert_impl; }
static BuiltinImpl dict_get_or_insert_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_get_or_insert_impl; }
static BuiltinImpl dict_insert_or_update_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_insert_or_update_impl; }
static BuiltinImpl dict_update_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_update_impl; }
static BuiltinImpl dict_swap_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_swap_impl; }
static BuiltinImpl dict_merge_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_merge_impl; }
static BuiltinImpl dict_delete_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_delete_impl; }
static BuiltinImpl dict_try_delete_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_try_delete_impl; }
static BuiltinImpl dict_pop_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_pop_impl; }
static BuiltinImpl dict_clear_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_clear_impl; }
static BuiltinImpl dict_union_in_place_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_union_in_place_impl; }
static BuiltinImpl dict_merge_all_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_merge_all_impl; }
static BuiltinImpl dict_keys_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_keys_impl; }
static BuiltinImpl dict_get_keys_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_get_keys_impl; }
static BuiltinImpl dict_for_each_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_for_each_impl; }
static BuiltinImpl dict_copy_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_copy_impl; }
static BuiltinImpl dict_map_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_map_impl; }
static BuiltinImpl dict_filter_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_filter_impl; }
static BuiltinImpl dict_filter_map_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_filter_map_impl; }
static BuiltinImpl dict_first_map_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_first_map_impl; }
static BuiltinImpl dict_map_reduce_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_map_reduce_impl; }
static BuiltinImpl dict_reduce_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_reduce_impl; }
static BuiltinImpl dict_to_array_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_to_array_impl; }
static BuiltinImpl dict_to_set_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_to_set_impl; }
static BuiltinImpl dict_to_dict_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_to_dict_impl; }
static BuiltinImpl dict_flatten_to_array_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_flatten_to_array_impl; }
static BuiltinImpl dict_flatten_to_set_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_flatten_to_set_impl; }
static BuiltinImpl dict_flatten_to_dict_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_flatten_to_dict_impl; }
static BuiltinImpl dict_group_fold_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return dict_group_fold_impl; }

/* --- registration --- */

void east_register_dict_builtins(BuiltinRegistry *reg) {
    builtin_registry_register(reg, "DictGenerate", dict_generate_factory);
    builtin_registry_register(reg, "DictSize", dict_size_factory);
    builtin_registry_register(reg, "DictHas", dict_has_factory);
    builtin_registry_register(reg, "DictGet", dict_get_factory);
    builtin_registry_register(reg, "DictGetOrDefault", dict_get_or_default_factory);
    builtin_registry_register(reg, "DictTryGet", dict_try_get_factory);
    builtin_registry_register(reg, "DictInsert", dict_insert_factory);
    builtin_registry_register(reg, "DictGetOrInsert", dict_get_or_insert_factory);
    builtin_registry_register(reg, "DictInsertOrUpdate", dict_insert_or_update_factory);
    builtin_registry_register(reg, "DictUpdate", dict_update_factory);
    builtin_registry_register(reg, "DictSwap", dict_swap_factory);
    builtin_registry_register(reg, "DictMerge", dict_merge_factory);
    builtin_registry_register(reg, "DictDelete", dict_delete_factory);
    builtin_registry_register(reg, "DictTryDelete", dict_try_delete_factory);
    builtin_registry_register(reg, "DictPop", dict_pop_factory);
    builtin_registry_register(reg, "DictClear", dict_clear_factory);
    builtin_registry_register(reg, "DictUnionInPlace", dict_union_in_place_factory);
    builtin_registry_register(reg, "DictMergeAll", dict_merge_all_factory);
    builtin_registry_register(reg, "DictKeys", dict_keys_factory);
    builtin_registry_register(reg, "DictGetKeys", dict_get_keys_factory);
    builtin_registry_register(reg, "DictForEach", dict_for_each_factory);
    builtin_registry_register(reg, "DictCopy", dict_copy_factory);
    builtin_registry_register(reg, "DictMap", dict_map_factory);
    builtin_registry_register(reg, "DictFilter", dict_filter_factory);
    builtin_registry_register(reg, "DictFilterMap", dict_filter_map_factory);
    builtin_registry_register(reg, "DictFirstMap", dict_first_map_factory);
    builtin_registry_register(reg, "DictMapReduce", dict_map_reduce_factory);
    builtin_registry_register(reg, "DictReduce", dict_reduce_factory);
    builtin_registry_register(reg, "DictToArray", dict_to_array_factory);
    builtin_registry_register(reg, "DictToSet", dict_to_set_factory);
    builtin_registry_register(reg, "DictToDict", dict_to_dict_factory);
    builtin_registry_register(reg, "DictFlattenToArray", dict_flatten_to_array_factory);
    builtin_registry_register(reg, "DictFlattenToSet", dict_flatten_to_set_factory);
    builtin_registry_register(reg, "DictFlattenToDict", dict_flatten_to_dict_factory);
    builtin_registry_register(reg, "DictGroupFold", dict_group_fold_factory);
}
