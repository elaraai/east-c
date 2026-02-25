/*
 * Array builtin functions.
 *
 * Many array operations take function-valued arguments (map, filter, fold, etc.).
 * These call through east_call() from compiler.h.
 */
#include "east/builtins.h"
#include "east/compiler.h"
#include "east/serialization.h"
#include "east/values.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helper: call a function value with given args                      */
/* ------------------------------------------------------------------ */
static EastValue *call_fn(EastValue *fn, EastValue **call_args, size_t nargs) {
    EvalResult r = east_call(fn->data.function.compiled, call_args, nargs);
    if (r.status == EVAL_OK || r.status == EVAL_RETURN) {
        return r.value;
    }
    /* Error -- free error details and return null */
    eval_result_free(&r);
    return east_null();
}

/* ================================================================== */
/* ArraySize                                                          */
/* ================================================================== */
static EastValue *array_size_impl(EastValue **args, size_t n) {
    (void)n;
    return east_integer((int64_t)east_array_len(args[0]));
}

/* ================================================================== */
/* ArrayHas                                                           */
/* ================================================================== */
static EastValue *array_has_impl(EastValue **args, size_t n) {
    (void)n;
    int64_t index = args[1]->data.integer;
    return east_boolean(index >= 0 && (size_t)index < east_array_len(args[0]));
}

/* ================================================================== */
/* ArrayGet                                                           */
/* ================================================================== */
static EastValue *array_get_impl(EastValue **args, size_t n) {
    (void)n;
    int64_t index = args[1]->data.integer;
    if (index < 0 || (size_t)index >= east_array_len(args[0])) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Array index out of bounds: index %lld, length %zu",
                 (long long)index, east_array_len(args[0]));
        east_builtin_error(msg);
        return NULL;
    }
    EastValue *v = east_array_get(args[0], (size_t)index);
    if (v) east_value_retain(v);
    return v;
}

/* ================================================================== */
/* ArrayGetOrDefault  (arr, index, default_fn)                        */
/* ================================================================== */
static EastValue *array_get_or_default_impl(EastValue **args, size_t n) {
    (void)n;
    int64_t index = args[1]->data.integer;
    if (index >= 0 && (size_t)index < east_array_len(args[0])) {
        EastValue *v = east_array_get(args[0], (size_t)index);
        if (v) east_value_retain(v);
        return v;
    }
    EastValue *call_args[] = { args[1] };
    return call_fn(args[2], call_args, 1);
}

/* ================================================================== */
/* ArrayTryGet  -> Option (variant: some/none)                        */
/* ================================================================== */
static EastValue *array_try_get_impl(EastValue **args, size_t n) {
    (void)n;
    int64_t index = args[1]->data.integer;
    if (index >= 0 && (size_t)index < east_array_len(args[0])) {
        EastValue *val = east_array_get(args[0], (size_t)index);
        return east_variant_new("some", val, NULL);
    }
    return east_variant_new("none", east_null(), NULL);
}

/* ================================================================== */
/* ArrayUpdate (arr, index, value) -> void (mutating)                 */
/* ================================================================== */
static EastValue *array_update_impl(EastValue **args, size_t n) {
    (void)n;
    int64_t index = args[1]->data.integer;
    EastValue *arr = args[0];
    size_t len = east_array_len(arr);
    if (index < 0 || (size_t)index >= len) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Array index %lld out of bounds",
                 (long long)index);
        east_builtin_error(msg);
        return NULL;
    }
    /* Direct mutation of the array items */
    east_value_retain(args[2]);
    EastValue *old = arr->data.array.items[(size_t)index];
    arr->data.array.items[(size_t)index] = args[2];
    east_value_release(old);
    return east_null();
}

/* ================================================================== */
/* ArrayPushLast                                                      */
/* ================================================================== */
static EastValue *array_push_last_impl(EastValue **args, size_t n) {
    (void)n;
    east_array_push(args[0], args[1]);
    return east_null();
}

/* ================================================================== */
/* ArrayPopLast                                                       */
/* ================================================================== */
static EastValue *array_pop_last_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    size_t len = east_array_len(arr);
    if (len == 0) {
        east_builtin_error("Cannot pop from empty Array");
        return NULL;
    }
    /* Transfer ownership from array to caller (no extra retain needed) */
    EastValue *val = arr->data.array.items[len - 1];
    arr->data.array.items[len - 1] = NULL;
    arr->data.array.len--;
    return val;
}

/* ================================================================== */
/* ArrayPushFirst                                                     */
/* ================================================================== */
static EastValue *array_push_first_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *val = args[1];
    size_t len = east_array_len(arr);
    /* Ensure capacity */
    east_array_push(arr, east_null()); /* grow */
    /* Shift elements right */
    for (size_t i = len; i > 0; i--) {
        arr->data.array.items[i] = arr->data.array.items[i - 1];
    }
    east_value_retain(val);
    arr->data.array.items[0] = val;
    return east_null();
}

/* ================================================================== */
/* ArrayPopFirst                                                      */
/* ================================================================== */
static EastValue *array_pop_first_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    size_t len = east_array_len(arr);
    if (len == 0) {
        east_builtin_error("Cannot pop from empty Array");
        return NULL;
    }
    /* Transfer ownership from array to caller (no extra retain needed) */
    EastValue *val = arr->data.array.items[0];
    for (size_t i = 0; i + 1 < len; i++) {
        arr->data.array.items[i] = arr->data.array.items[i + 1];
    }
    arr->data.array.items[len - 1] = NULL;
    arr->data.array.len--;
    return val;
}

/* ================================================================== */
/* ArraySlice                                                         */
/* ================================================================== */
static EastValue *array_slice_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    int64_t start = args[1]->data.integer;
    int64_t end = args[2]->data.integer;
    size_t len = east_array_len(arr);
    if (start < 0) start = 0;
    if ((size_t)end > len) end = (int64_t)len;
    if (start >= end) {
        return east_array_new(arr->data.array.elem_type);
    }
    EastValue *result = east_array_new(arr->data.array.elem_type);
    for (int64_t i = start; i < end; i++) {
        east_array_push(result, east_array_get(arr, (size_t)i));
    }
    return result;
}

/* ================================================================== */
/* ArrayConcat                                                        */
/* ================================================================== */
static EastValue *array_concat_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *a = args[0];
    EastValue *b = args[1];
    EastValue *result = east_array_new(a->data.array.elem_type);
    for (size_t i = 0; i < east_array_len(a); i++)
        east_array_push(result, east_array_get(a, i));
    for (size_t i = 0; i < east_array_len(b); i++)
        east_array_push(result, east_array_get(b, i));
    return result;
}

/* ================================================================== */
/* ArrayReverse                                                       */
/* ================================================================== */
static EastValue *array_reverse_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    size_t len = east_array_len(arr);
    EastValue *result = east_array_new(arr->data.array.elem_type);
    for (size_t i = 0; i < len; i++)
        east_array_push(result, east_array_get(arr, len - 1 - i));
    return result;
}

/* ================================================================== */
/* ArrayClear                                                         */
/* ================================================================== */
static EastValue *array_clear_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    /* Release all elements */
    for (size_t i = 0; i < east_array_len(arr); i++) {
        east_value_release(arr->data.array.items[i]);
    }
    arr->data.array.len = 0;
    return east_null();
}

/* ================================================================== */
/* ArrayCopy                                                          */
/* ================================================================== */
static EastValue *array_copy_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *result = east_array_new(arr->data.array.elem_type);
    for (size_t i = 0; i < east_array_len(arr); i++)
        east_array_push(result, east_array_get(arr, i));
    return result;
}

/* ================================================================== */
/* ArrayReverseInPlace                                                */
/* ================================================================== */
static EastValue *array_reverse_in_place_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    size_t len = east_array_len(arr);
    for (size_t i = 0; i < len / 2; i++) {
        EastValue *tmp = arr->data.array.items[i];
        arr->data.array.items[i] = arr->data.array.items[len - 1 - i];
        arr->data.array.items[len - 1 - i] = tmp;
    }
    return east_null();
}

/* ================================================================== */
/* ArrayRange (start, end, step)                                      */
/* ================================================================== */
static EastValue *array_range_impl(EastValue **args, size_t n) {
    (void)n;
    int64_t start = args[0]->data.integer;
    int64_t end = args[1]->data.integer;
    int64_t step = args[2]->data.integer;
    EastValue *result = east_array_new(&east_integer_type);
    if (step > 0) {
        for (int64_t i = start; i < end; i += step) {
            EastValue *v = east_integer(i);
            east_array_push(result, v);
            east_value_release(v);
        }
    } else if (step < 0) {
        for (int64_t i = start; i > end; i += step) {
            EastValue *v = east_integer(i);
            east_array_push(result, v);
            east_value_release(v);
        }
    }
    return result;
}

/* ================================================================== */
/* ArrayLinspace (start, end, n)                                      */
/* ================================================================== */
static EastValue *array_linspace_impl(EastValue **args, size_t n_args) {
    (void)n_args;
    double start = args[0]->data.float64;
    double end = args[1]->data.float64;
    int64_t count = args[2]->data.integer;
    EastValue *result = east_array_new(&east_float_type);
    if (count <= 0) return result;
    if (count == 1) {
        EastValue *v = east_float(start);
        east_array_push(result, v);
        east_value_release(v);
        return result;
    }
    double step = (end - start) / (double)(count - 1);
    for (int64_t i = 0; i < count; i++) {
        EastValue *v = east_float(start + (double)i * step);
        east_array_push(result, v);
        east_value_release(v);
    }
    return result;
}

/* ================================================================== */
/* ArrayMap (arr, fn) -> new array                                    */
/* ================================================================== */
static EastValue *array_map_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *fn = args[1];
    size_t len = east_array_len(arr);
    /* We use type_params[1] as elem_type for the result if available,
       otherwise fall back to the source array's elem type */
    EastValue *result = east_array_new(arr->data.array.elem_type);
    for (size_t i = 0; i < len; i++) {
        EastValue *idx = east_integer((int64_t)i);
        EastValue *call_args[] = { east_array_get(arr, i), idx };
        EastValue *mapped = call_fn(fn, call_args, 2);
        east_array_push(result, mapped);
        east_value_release(mapped);
        east_value_release(idx);
    }
    return result;
}

/* ================================================================== */
/* ArrayFilter (arr, fn) -> new array                                 */
/* ================================================================== */
static EastValue *array_filter_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *fn = args[1];
    size_t len = east_array_len(arr);
    EastValue *result = east_array_new(arr->data.array.elem_type);
    for (size_t i = 0; i < len; i++) {
        EastValue *item = east_array_get(arr, i);
        EastValue *idx = east_integer((int64_t)i);
        EastValue *call_args[] = { item, idx };
        EastValue *pred = call_fn(fn, call_args, 2);
        if (pred->data.boolean) {
            east_array_push(result, item);
        }
        east_value_release(idx);
        east_value_release(pred);
    }
    return result;
}

/* ================================================================== */
/* ArrayFold (arr, initial, fn) -> value                              */
/* ================================================================== */
static EastValue *array_fold_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *acc = args[1];
    EastValue *fn = args[2];
    size_t len = east_array_len(arr);
    east_value_retain(acc);
    for (size_t i = 0; i < len; i++) {
        EastValue *idx = east_integer((int64_t)i);
        EastValue *call_args[] = { acc, east_array_get(arr, i), idx };
        EastValue *new_acc = call_fn(fn, call_args, 3);
        east_value_release(acc);
        acc = new_acc;
        east_value_retain(acc);
        east_value_release(new_acc);
        east_value_release(idx);
    }
    return acc;
}

/* ================================================================== */
/* ArrayGenerate (n, fn) -> new array                                 */
/* ================================================================== */
static EastValue *array_generate_impl(EastValue **args, size_t n) {
    (void)n;
    int64_t count = args[0]->data.integer;
    EastValue *fn = args[1];
    /* Use the type from type_params if available via factory */
    EastValue *result = east_array_new(&east_null_type);
    for (int64_t i = 0; i < count; i++) {
        EastValue *idx = east_integer(i);
        EastValue *call_args[] = { idx };
        EastValue *val = call_fn(fn, call_args, 1);
        east_array_push(result, val);
        east_value_release(val);
        east_value_release(idx);
    }
    return result;
}

/* ================================================================== */
/* ArraySort (arr, key_fn) -> new sorted array                        */
/* ================================================================== */

/* Comparison context for qsort_r-style sorting */
typedef struct {
    EastValue **keys;
} SortCtx;

static int sort_compare(const void *a, const void *b, void *ctx) {
    SortCtx *sc = (SortCtx *)ctx;
    size_t ia = *(const size_t *)a;
    size_t ib = *(const size_t *)b;
    return east_value_compare(sc->keys[ia], sc->keys[ib]);
}

/* Portable sort since qsort_r is not universally available */
static _Thread_local SortCtx *g_sort_ctx = NULL;
static int sort_compare_global(const void *a, const void *b) {
    size_t ia = *(const size_t *)a;
    size_t ib = *(const size_t *)b;
    return east_value_compare(g_sort_ctx->keys[ia], g_sort_ctx->keys[ib]);
}

static EastValue *array_sort_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *key_fn = args[1];
    size_t len = east_array_len(arr);
    if (len == 0) return east_array_new(arr->data.array.elem_type);

    /* Compute keys */
    EastValue **keys = malloc(len * sizeof(EastValue *));
    size_t *indices = malloc(len * sizeof(size_t));
    for (size_t i = 0; i < len; i++) {
        indices[i] = i;
        EastValue *call_args[] = { east_array_get(arr, i) };
        keys[i] = call_fn(key_fn, call_args, 1);
    }

    SortCtx ctx = { .keys = keys };
    g_sort_ctx = &ctx;
    qsort(indices, len, sizeof(size_t), sort_compare_global);
    g_sort_ctx = NULL;

    EastValue *result = east_array_new(arr->data.array.elem_type);
    for (size_t i = 0; i < len; i++)
        east_array_push(result, east_array_get(arr, indices[i]));

    for (size_t i = 0; i < len; i++) east_value_release(keys[i]);
    free(keys);
    free(indices);
    return result;
}

/* ================================================================== */
/* ArraySortInPlace (arr, key_fn) -> void                             */
/* ================================================================== */
static EastValue *array_sort_in_place_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *key_fn = args[1];
    size_t len = east_array_len(arr);
    if (len <= 1) return east_null();

    EastValue **keys = malloc(len * sizeof(EastValue *));
    size_t *indices = malloc(len * sizeof(size_t));
    for (size_t i = 0; i < len; i++) {
        indices[i] = i;
        EastValue *call_args[] = { east_array_get(arr, i) };
        keys[i] = call_fn(key_fn, call_args, 1);
    }

    SortCtx ctx = { .keys = keys };
    g_sort_ctx = &ctx;
    qsort(indices, len, sizeof(size_t), sort_compare_global);
    g_sort_ctx = NULL;

    /* Reorder in-place */
    EastValue **tmp = malloc(len * sizeof(EastValue *));
    for (size_t i = 0; i < len; i++)
        tmp[i] = arr->data.array.items[indices[i]];
    memcpy(arr->data.array.items, tmp, len * sizeof(EastValue *));

    for (size_t i = 0; i < len; i++) east_value_release(keys[i]);
    free(keys);
    free(indices);
    free(tmp);
    return east_null();
}

/* ================================================================== */
/* ArrayIsSorted (arr, key_fn) -> bool                                */
/* ================================================================== */
static EastValue *array_is_sorted_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *key_fn = args[1];
    size_t len = east_array_len(arr);
    if (len <= 1) return east_boolean(true);

    EastValue *prev_key;
    {
        EastValue *call_args[] = { east_array_get(arr, 0) };
        prev_key = call_fn(key_fn, call_args, 1);
    }
    for (size_t i = 1; i < len; i++) {
        EastValue *call_args[] = { east_array_get(arr, i) };
        EastValue *key = call_fn(key_fn, call_args, 1);
        if (east_value_compare(prev_key, key) > 0) {
            east_value_release(prev_key);
            east_value_release(key);
            return east_boolean(false);
        }
        east_value_release(prev_key);
        prev_key = key;
    }
    east_value_release(prev_key);
    return east_boolean(true);
}

/* ================================================================== */
/* ArrayFindSortedFirst (arr, target, key_fn) -> int                  */
/* ================================================================== */
static EastValue *array_find_sorted_first_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *target = args[1];
    EastValue *key_fn = args[2];
    size_t left = 0, right = east_array_len(arr);
    while (left < right) {
        size_t mid = (left + right) / 2;
        EastValue *call_args[] = { east_array_get(arr, mid) };
        EastValue *key = call_fn(key_fn, call_args, 1);
        if (east_value_compare(key, target) < 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
        east_value_release(key);
    }
    return east_integer((int64_t)left);
}

/* ================================================================== */
/* ArrayFindSortedLast (arr, target, key_fn) -> int                   */
/* ================================================================== */
static EastValue *array_find_sorted_last_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *target = args[1];
    EastValue *key_fn = args[2];
    size_t left = 0, right = east_array_len(arr);
    while (left < right) {
        size_t mid = (left + right) / 2;
        EastValue *call_args[] = { east_array_get(arr, mid) };
        EastValue *key = call_fn(key_fn, call_args, 1);
        if (east_value_compare(key, target) <= 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
        east_value_release(key);
    }
    return east_integer((int64_t)left);
}

/* ================================================================== */
/* ArrayFindSortedRange (arr, target, key_fn) -> struct{start,end}    */
/* ================================================================== */
static EastValue *array_find_sorted_range_impl(EastValue **args, size_t n) {
    EastValue *first = array_find_sorted_first_impl(args, n);
    EastValue *last = array_find_sorted_last_impl(args, n);
    const char *names[] = { "start", "end" };
    EastValue *vals[] = { first, last };
    EastValue *result = east_struct_new(names, vals, 2, NULL);
    east_value_release(first);
    east_value_release(last);
    return result;
}

/* ================================================================== */
/* ArrayFindFirst (arr, target, key_fn) -> Option<Integer>            */
/* ================================================================== */
static EastValue *array_find_first_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *target = args[1];
    EastValue *key_fn = args[2];
    size_t len = east_array_len(arr);
    for (size_t i = 0; i < len; i++) {
        EastValue *call_args[] = { east_array_get(arr, i) };
        EastValue *key = call_fn(key_fn, call_args, 1);
        if (east_value_compare(key, target) == 0) {
            east_value_release(key);
            return east_variant_new("some", east_integer((int64_t)i), NULL);
        }
        east_value_release(key);
    }
    return east_variant_new("none", east_null(), NULL);
}

/* ================================================================== */
/* ArrayGetKeys (arr, indices, default_fn) -> array                   */
/* ================================================================== */
static EastValue *array_get_keys_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *indices = args[1];
    EastValue *default_fn = args[2];
    size_t arr_len = east_array_len(arr);
    size_t idx_len = east_array_len(indices);
    EastValue *result = east_array_new(arr->data.array.elem_type);
    for (size_t i = 0; i < idx_len; i++) {
        int64_t index = east_array_get(indices, i)->data.integer;
        if (index >= 0 && (size_t)index < arr_len) {
            east_array_push(result, east_array_get(arr, (size_t)index));
        } else {
            EastValue *call_args[] = { east_integer(index) };
            EastValue *def = call_fn(default_fn, call_args, 1);
            east_array_push(result, def);
            east_value_release(def);
            east_value_release(call_args[0]);
        }
    }
    return result;
}

/* ================================================================== */
/* ArrayForEach (arr, fn) -> void                                     */
/* ================================================================== */
static EastValue *array_for_each_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *fn = args[1];
    size_t len = east_array_len(arr);
    for (size_t i = 0; i < len; i++) {
        EastValue *idx = east_integer((int64_t)i);
        EastValue *call_args[] = { east_array_get(arr, i), idx };
        EastValue *ret = call_fn(fn, call_args, 2);
        east_value_release(ret);
        east_value_release(idx);
    }
    return east_null();
}

/* ================================================================== */
/* ArrayFilterMap (arr, fn) -> array  (fn returns Option)             */
/* ================================================================== */
static EastValue *array_filter_map_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *fn = args[1];
    size_t len = east_array_len(arr);
    EastValue *result = east_array_new(arr->data.array.elem_type);
    for (size_t i = 0; i < len; i++) {
        EastValue *idx = east_integer((int64_t)i);
        EastValue *call_args[] = { east_array_get(arr, i), idx };
        EastValue *opt = call_fn(fn, call_args, 2);
        /* Check if variant is "some" */
        if (opt->kind == EAST_VAL_VARIANT && strcmp(opt->data.variant.case_name, "some") == 0) {
            east_array_push(result, opt->data.variant.value);
        }
        east_value_release(idx);
        east_value_release(opt);
    }
    return result;
}

/* ================================================================== */
/* ArrayFirstMap (arr, fn) -> Option                                  */
/* ================================================================== */
static EastValue *array_first_map_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *fn = args[1];
    size_t len = east_array_len(arr);
    for (size_t i = 0; i < len; i++) {
        EastValue *idx = east_integer((int64_t)i);
        EastValue *call_args[] = { east_array_get(arr, i), idx };
        EastValue *opt = call_fn(fn, call_args, 2);
        east_value_release(idx);
        if (opt->kind == EAST_VAL_VARIANT && strcmp(opt->data.variant.case_name, "some") == 0) {
            return opt;
        }
        east_value_release(opt);
    }
    return east_variant_new("none", east_null(), NULL);
}

/* ================================================================== */
/* ArrayMapReduce (arr, map_fn, reduce_fn) -> value                   */
/* ================================================================== */
static EastValue *array_map_reduce_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *map_fn = args[1];
    EastValue *reduce_fn = args[2];
    size_t len = east_array_len(arr);
    if (len == 0) {
        east_builtin_error("Cannot reduce empty array with no initial value");
        return NULL;
    }

    /* Map first element */
    EastValue *idx0 = east_integer(0);
    EastValue *map_args0[] = { east_array_get(arr, 0), idx0 };
    EastValue *acc = call_fn(map_fn, map_args0, 2);
    east_value_release(idx0);

    for (size_t i = 1; i < len; i++) {
        EastValue *idx = east_integer((int64_t)i);
        EastValue *map_args[] = { east_array_get(arr, i), idx };
        EastValue *mapped = call_fn(map_fn, map_args, 2);
        east_value_release(idx);

        EastValue *reduce_args[] = { acc, mapped };
        EastValue *new_acc = call_fn(reduce_fn, reduce_args, 2);
        east_value_release(acc);
        east_value_release(mapped);
        acc = new_acc;
    }
    return acc;
}

/* ================================================================== */
/* ArrayMerge (arr, index, value, fn) -> void                         */
/* ================================================================== */
static EastValue *array_merge_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    int64_t index = args[1]->data.integer;
    EastValue *value = args[2];
    EastValue *fn = args[3];
    size_t len = east_array_len(arr);
    if (index < 0 || (size_t)index >= len) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Array index %lld out of bounds",
                 (long long)index);
        east_builtin_error(msg);
        return NULL;
    }
    EastValue *old = east_array_get(arr, (size_t)index);
    EastValue *idx = east_integer(index);
    EastValue *call_args[] = { old, value, idx };
    EastValue *merged = call_fn(fn, call_args, 3);
    /* call_fn returns owned; store directly in slot (transfers ownership) */
    EastValue *prev = arr->data.array.items[(size_t)index];
    arr->data.array.items[(size_t)index] = merged;
    east_value_release(prev);
    east_value_release(idx);
    return east_null();
}

/* ================================================================== */
/* ArrayAppend (arr, other) -> void                                   */
/* ================================================================== */
static EastValue *array_append_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *other = args[1];
    for (size_t i = 0; i < east_array_len(other); i++)
        east_array_push(arr, east_array_get(other, i));
    return east_null();
}

/* ================================================================== */
/* ArrayPrepend (arr, other) -> void                                  */
/* ================================================================== */
static EastValue *array_prepend_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *other = args[1];
    size_t other_len = east_array_len(other);
    /* Insert each element of other at position i */
    for (size_t i = 0; i < other_len; i++) {
        EastValue *val = east_array_get(other, i);
        /* Grow array and shift */
        size_t len = east_array_len(arr);
        east_array_push(arr, east_null());
        for (size_t j = len; j > i; j--) {
            arr->data.array.items[j] = arr->data.array.items[j - 1];
        }
        east_value_retain(val);
        arr->data.array.items[i] = val;
    }
    return east_null();
}

/* ================================================================== */
/* ArrayMergeAll (arr, other, fn) -> void                             */
/* ================================================================== */
static EastValue *array_merge_all_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *other = args[1];
    EastValue *fn = args[2];
    size_t arr_len = east_array_len(arr);
    size_t other_len = east_array_len(other);
    for (size_t i = 0; i < other_len && i < arr_len; i++) {
        EastValue *idx = east_integer((int64_t)i);
        EastValue *call_args[] = { east_array_get(arr, i), east_array_get(other, i), idx };
        EastValue *merged = call_fn(fn, call_args, 3);
        /* call_fn returns owned; store directly in slot (transfers ownership) */
        EastValue *prev = arr->data.array.items[i];
        arr->data.array.items[i] = merged;
        east_value_release(prev);
        east_value_release(idx);
    }
    return east_null();
}

/* ================================================================== */
/* ArrayStringJoin (arr, delimiter) -> string                         */
/* ================================================================== */
static EastValue *array_string_join_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    const char *delim = args[1]->data.string.data;
    size_t dlen = args[1]->data.string.len;
    size_t len = east_array_len(arr);
    if (len == 0) return east_string("");

    /* Calculate total length */
    size_t total = 0;
    for (size_t i = 0; i < len; i++) {
        total += east_array_get(arr, i)->data.string.len;
        if (i > 0) total += dlen;
    }
    char *buf = malloc(total + 1);
    if (!buf) return east_string("");
    char *dst = buf;
    for (size_t i = 0; i < len; i++) {
        if (i > 0) { memcpy(dst, delim, dlen); dst += dlen; }
        EastValue *s = east_array_get(arr, i);
        memcpy(dst, s->data.string.data, s->data.string.len);
        dst += s->data.string.len;
    }
    *dst = '\0';
    EastValue *result = east_string_len(buf, total);
    free(buf);
    return result;
}

/* ================================================================== */
/* ArrayToSet (arr, key_fn) -> set                                    */
/* ================================================================== */
static EastValue *array_to_set_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *key_fn = args[1];
    size_t len = east_array_len(arr);
    EastValue *result = east_set_new(arr->data.array.elem_type);
    for (size_t i = 0; i < len; i++) {
        EastValue *idx = east_integer((int64_t)i);
        EastValue *call_args[] = { east_array_get(arr, i), idx };
        EastValue *key = call_fn(key_fn, call_args, 2);
        east_set_insert(result, key);
        east_value_release(key);
        east_value_release(idx);
    }
    return result;
}

/* ================================================================== */
/* ArrayToDict (arr, key_fn, value_fn, merge_fn) -> dict              */
/* ================================================================== */
static EastValue *array_to_dict_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *key_fn = args[1];
    EastValue *value_fn = args[2];
    EastValue *merge_fn = args[3];
    size_t len = east_array_len(arr);
    EastValue *result = east_dict_new(&east_null_type, &east_null_type);
    for (size_t i = 0; i < len; i++) {
        EastValue *idx = east_integer((int64_t)i);
        EastValue *kargs[] = { east_array_get(arr, i), idx };
        EastValue *key = call_fn(key_fn, kargs, 2);
        EastValue *vargs[] = { east_array_get(arr, i), idx };
        EastValue *val = call_fn(value_fn, vargs, 2);
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

/* ================================================================== */
/* ArrayFlattenToArray (arr, fn) -> array                             */
/* ================================================================== */
static EastValue *array_flatten_to_array_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *fn = args[1];
    size_t len = east_array_len(arr);
    EastValue *result = east_array_new(&east_null_type);
    for (size_t i = 0; i < len; i++) {
        EastValue *idx = east_integer((int64_t)i);
        EastValue *call_args[] = { east_array_get(arr, i), idx };
        EastValue *mapped = call_fn(fn, call_args, 2);
        /* mapped should be an array -- flatten it */
        for (size_t j = 0; j < east_array_len(mapped); j++)
            east_array_push(result, east_array_get(mapped, j));
        east_value_release(mapped);
        east_value_release(idx);
    }
    return result;
}

/* ================================================================== */
/* ArrayFlattenToSet (arr, fn) -> set                                 */
/* ================================================================== */
static EastValue *array_flatten_to_set_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *fn = args[1];
    size_t len = east_array_len(arr);
    EastValue *result = east_set_new(&east_null_type);
    for (size_t i = 0; i < len; i++) {
        EastValue *idx = east_integer((int64_t)i);
        EastValue *call_args[] = { east_array_get(arr, i), idx };
        EastValue *mapped = call_fn(fn, call_args, 2);
        /* mapped is a set -- iterate and insert */
        if (mapped->kind == EAST_VAL_SET) {
            for (size_t j = 0; j < mapped->data.set.len; j++)
                east_set_insert(result, mapped->data.set.items[j]);
        }
        east_value_release(mapped);
        east_value_release(idx);
    }
    return result;
}

/* ================================================================== */
/* ArrayFlattenToDict (arr, fn, merge_fn) -> dict                     */
/* ================================================================== */
static EastValue *array_flatten_to_dict_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *fn = args[1];
    EastValue *merge_fn = args[2];
    size_t len = east_array_len(arr);
    EastValue *result = east_dict_new(&east_null_type, &east_null_type);
    for (size_t i = 0; i < len; i++) {
        EastValue *idx = east_integer((int64_t)i);
        EastValue *call_args[] = { east_array_get(arr, i), idx };
        EastValue *mapped = call_fn(fn, call_args, 2);
        /* mapped is a dict -- merge each key/value */
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
        east_value_release(idx);
    }
    return result;
}

/* ================================================================== */
/* ArrayGroupFold (arr, key_fn, init_fn, fold_fn) -> dict             */
/* ================================================================== */
static EastValue *array_group_fold_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    EastValue *key_fn = args[1];
    EastValue *init_fn = args[2];
    EastValue *fold_fn = args[3];
    size_t len = east_array_len(arr);
    EastValue *result = east_dict_new(&east_null_type, &east_null_type);
    for (size_t i = 0; i < len; i++) {
        EastValue *idx = east_integer((int64_t)i);
        EastValue *kargs[] = { east_array_get(arr, i), idx };
        EastValue *key = call_fn(key_fn, kargs, 2);
        EastValue *acc;
        if (!east_dict_has(result, key)) {
            EastValue *iargs[] = { key };
            acc = call_fn(init_fn, iargs, 1);
            east_dict_set(result, key, acc);
            east_value_release(acc);
        } else {
            acc = east_dict_get(result, key);
        }
        EastValue *fargs[] = { acc, east_array_get(arr, i), idx };
        EastValue *new_acc = call_fn(fold_fn, fargs, 3);
        east_dict_set(result, key, new_acc);
        east_value_release(new_acc);
        east_value_release(key);
        east_value_release(idx);
    }
    return result;
}

/* ================================================================== */
/* ArrayEncodeCsv                                                     */
/* ================================================================== */

static _Thread_local EastType *csv_encode_struct_type_ctx = NULL;

static EastValue *array_encode_csv_impl2(EastValue **args, size_t n) {
    EastType *struct_type = csv_encode_struct_type_ctx;
    if (!struct_type) {
        east_builtin_error("CSV encode: no type context");
        return NULL;
    }

    /* Build array type from struct type */
    EastType *arr_type = east_array_type(struct_type);

    /* args[0] = array, args[1] = config (optional) */
    EastValue *config = (n > 1) ? args[1] : NULL;
    char *csv_str = east_csv_encode(args[0], arr_type, config);
    east_type_release(arr_type);

    if (!csv_str) return east_blob(NULL, 0);

    EastValue *result = east_blob((const uint8_t *)csv_str, strlen(csv_str));
    free(csv_str);
    return result;
}

/* --- factory functions --- */

static BuiltinImpl array_generate_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_generate_impl; }
static BuiltinImpl array_range_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_range_impl; }
static BuiltinImpl array_linspace_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_linspace_impl; }
static BuiltinImpl array_size_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_size_impl; }
static BuiltinImpl array_has_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_has_impl; }
static BuiltinImpl array_get_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_get_impl; }
static BuiltinImpl array_get_or_default_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_get_or_default_impl; }
static BuiltinImpl array_try_get_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_try_get_impl; }
static BuiltinImpl array_update_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_update_impl; }
static BuiltinImpl array_merge_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_merge_impl; }
static BuiltinImpl array_push_last_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_push_last_impl; }
static BuiltinImpl array_pop_last_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_pop_last_impl; }
static BuiltinImpl array_push_first_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_push_first_impl; }
static BuiltinImpl array_pop_first_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_pop_first_impl; }
static BuiltinImpl array_append_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_append_impl; }
static BuiltinImpl array_prepend_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_prepend_impl; }
static BuiltinImpl array_merge_all_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_merge_all_impl; }
static BuiltinImpl array_clear_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_clear_impl; }
static BuiltinImpl array_sort_in_place_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_sort_in_place_impl; }
static BuiltinImpl array_reverse_in_place_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_reverse_in_place_impl; }
static BuiltinImpl array_sort_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_sort_impl; }
static BuiltinImpl array_reverse_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_reverse_impl; }
static BuiltinImpl array_is_sorted_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_is_sorted_impl; }
static BuiltinImpl array_find_sorted_first_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_find_sorted_first_impl; }
static BuiltinImpl array_find_sorted_last_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_find_sorted_last_impl; }
static BuiltinImpl array_find_sorted_range_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_find_sorted_range_impl; }
static BuiltinImpl array_find_first_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_find_first_impl; }
static BuiltinImpl array_concat_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_concat_impl; }
static BuiltinImpl array_slice_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_slice_impl; }
static BuiltinImpl array_get_keys_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_get_keys_impl; }
static BuiltinImpl array_for_each_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_for_each_impl; }
static BuiltinImpl array_copy_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_copy_impl; }
static BuiltinImpl array_map_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_map_impl; }
static BuiltinImpl array_filter_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_filter_impl; }
static BuiltinImpl array_filter_map_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_filter_map_impl; }
static BuiltinImpl array_first_map_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_first_map_impl; }
static BuiltinImpl array_map_reduce_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_map_reduce_impl; }
static BuiltinImpl array_fold_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_fold_impl; }
static BuiltinImpl array_string_join_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_string_join_impl; }
static BuiltinImpl array_to_set_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_to_set_impl; }
static BuiltinImpl array_to_dict_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_to_dict_impl; }
static BuiltinImpl array_flatten_to_array_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_flatten_to_array_impl; }
static BuiltinImpl array_flatten_to_set_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_flatten_to_set_impl; }
static BuiltinImpl array_flatten_to_dict_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_flatten_to_dict_impl; }
static BuiltinImpl array_group_fold_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return array_group_fold_impl; }
static BuiltinImpl array_encode_csv_factory(EastType **tp, size_t ntp) {
    csv_encode_struct_type_ctx = (ntp > 0) ? tp[0] : NULL;
    return array_encode_csv_impl2;
}

/* --- registration --- */

void east_register_array_builtins(BuiltinRegistry *reg) {
    builtin_registry_register(reg, "ArrayGenerate", array_generate_factory);
    builtin_registry_register(reg, "ArrayRange", array_range_factory);
    builtin_registry_register(reg, "ArrayLinspace", array_linspace_factory);
    builtin_registry_register(reg, "ArraySize", array_size_factory);
    builtin_registry_register(reg, "ArrayHas", array_has_factory);
    builtin_registry_register(reg, "ArrayGet", array_get_factory);
    builtin_registry_register(reg, "ArrayGetOrDefault", array_get_or_default_factory);
    builtin_registry_register(reg, "ArrayTryGet", array_try_get_factory);
    builtin_registry_register(reg, "ArrayUpdate", array_update_factory);
    builtin_registry_register(reg, "ArrayMerge", array_merge_factory);
    builtin_registry_register(reg, "ArrayPushLast", array_push_last_factory);
    builtin_registry_register(reg, "ArrayPopLast", array_pop_last_factory);
    builtin_registry_register(reg, "ArrayPushFirst", array_push_first_factory);
    builtin_registry_register(reg, "ArrayPopFirst", array_pop_first_factory);
    builtin_registry_register(reg, "ArrayAppend", array_append_factory);
    builtin_registry_register(reg, "ArrayPrepend", array_prepend_factory);
    builtin_registry_register(reg, "ArrayMergeAll", array_merge_all_factory);
    builtin_registry_register(reg, "ArrayClear", array_clear_factory);
    builtin_registry_register(reg, "ArraySortInPlace", array_sort_in_place_factory);
    builtin_registry_register(reg, "ArrayReverseInPlace", array_reverse_in_place_factory);
    builtin_registry_register(reg, "ArraySort", array_sort_factory);
    builtin_registry_register(reg, "ArrayReverse", array_reverse_factory);
    builtin_registry_register(reg, "ArrayIsSorted", array_is_sorted_factory);
    builtin_registry_register(reg, "ArrayFindSortedFirst", array_find_sorted_first_factory);
    builtin_registry_register(reg, "ArrayFindSortedLast", array_find_sorted_last_factory);
    builtin_registry_register(reg, "ArrayFindSortedRange", array_find_sorted_range_factory);
    builtin_registry_register(reg, "ArrayFindFirst", array_find_first_factory);
    builtin_registry_register(reg, "ArrayConcat", array_concat_factory);
    builtin_registry_register(reg, "ArraySlice", array_slice_factory);
    builtin_registry_register(reg, "ArrayGetKeys", array_get_keys_factory);
    builtin_registry_register(reg, "ArrayForEach", array_for_each_factory);
    builtin_registry_register(reg, "ArrayCopy", array_copy_factory);
    builtin_registry_register(reg, "ArrayMap", array_map_factory);
    builtin_registry_register(reg, "ArrayFilter", array_filter_factory);
    builtin_registry_register(reg, "ArrayFilterMap", array_filter_map_factory);
    builtin_registry_register(reg, "ArrayFirstMap", array_first_map_factory);
    builtin_registry_register(reg, "ArrayMapReduce", array_map_reduce_factory);
    builtin_registry_register(reg, "ArrayFold", array_fold_factory);
    builtin_registry_register(reg, "ArrayStringJoin", array_string_join_factory);
    builtin_registry_register(reg, "ArrayToSet", array_to_set_factory);
    builtin_registry_register(reg, "ArrayToDict", array_to_dict_factory);
    builtin_registry_register(reg, "ArrayFlattenToArray", array_flatten_to_array_factory);
    builtin_registry_register(reg, "ArrayFlattenToSet", array_flatten_to_set_factory);
    builtin_registry_register(reg, "ArrayFlattenToDict", array_flatten_to_dict_factory);
    builtin_registry_register(reg, "ArrayGroupFold", array_group_fold_factory);
    builtin_registry_register(reg, "ArrayEncodeCsv", array_encode_csv_factory);
}
