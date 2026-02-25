/*
 * Vector builtin functions.
 *
 * Vectors store homogeneous numeric data (float64, int64, or bool) in
 * a contiguous buffer (data.vector.data). The element type determines
 * how we index into the buffer.
 */
#include "east/builtins.h"
#include "east/compiler.h"
#include "east/values.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helper: call a function value                                      */
/* ------------------------------------------------------------------ */
static EastValue *call_fn(EastValue *fn, EastValue **call_args, size_t nargs) {
    EvalResult r = east_call(fn->data.function.compiled, call_args, nargs);
    if (r.status == EVAL_OK || r.status == EVAL_RETURN) return r.value;
    eval_result_free(&r);
    return east_null();
}

/* ------------------------------------------------------------------ */
/* Helpers: read/write vector elements based on elem_type              */
/* ------------------------------------------------------------------ */
static EastValue *vec_get_elem(EastValue *vec, size_t i) {
    EastType *et = vec->data.vector.elem_type;
    void *data = vec->data.vector.data;
    if (et->kind == EAST_TYPE_FLOAT) {
        return east_float(((double *)data)[i]);
    } else if (et->kind == EAST_TYPE_INTEGER) {
        return east_integer(((int64_t *)data)[i]);
    } else if (et->kind == EAST_TYPE_BOOLEAN) {
        return east_boolean(((bool *)data)[i]);
    }
    return east_null();
}

static void vec_set_elem(EastValue *vec, size_t i, EastValue *val) {
    EastType *et = vec->data.vector.elem_type;
    void *data = vec->data.vector.data;
    if (et->kind == EAST_TYPE_FLOAT) {
        ((double *)data)[i] = val->data.float64;
    } else if (et->kind == EAST_TYPE_INTEGER) {
        ((int64_t *)data)[i] = val->data.integer;
    } else if (et->kind == EAST_TYPE_BOOLEAN) {
        ((bool *)data)[i] = val->data.boolean;
    }
}

static size_t elem_size(EastType *et) {
    if (et->kind == EAST_TYPE_FLOAT) return sizeof(double);
    if (et->kind == EAST_TYPE_INTEGER) return sizeof(int64_t);
    if (et->kind == EAST_TYPE_BOOLEAN) return sizeof(bool);
    return sizeof(double);
}

/* --- implementations --- */

static EastValue *vector_length_impl(EastValue **args, size_t n) {
    (void)n;
    return east_integer((int64_t)args[0]->data.vector.len);
}

static EastValue *vector_get_impl(EastValue **args, size_t n) {
    (void)n;
    int64_t idx = args[1]->data.integer;
    size_t len = args[0]->data.vector.len;
    if (idx < 0 || (size_t)idx >= len) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Vector index %lld out of bounds (length %zu)",
                 (long long)idx, len);
        east_builtin_error(msg);
        return NULL;
    }
    return vec_get_elem(args[0], (size_t)idx);
}

static EastValue *vector_set_impl(EastValue **args, size_t n) {
    (void)n;
    int64_t idx = args[1]->data.integer;
    size_t len = args[0]->data.vector.len;
    if (idx < 0 || (size_t)idx >= len) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Vector index %lld out of bounds (length %zu)",
                 (long long)idx, len);
        east_builtin_error(msg);
        return NULL;
    }
    vec_set_elem(args[0], (size_t)idx, args[2]);
    return east_null();
}

static EastValue *vector_slice_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *vec = args[0];
    int64_t start = args[1]->data.integer;
    int64_t end = args[2]->data.integer;
    size_t len = vec->data.vector.len;
    if (start < 0 || end > (int64_t)len || start > end) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Vector slice [%lld, %lld) out of bounds (length %zu)",
                 (long long)start, (long long)end, len);
        east_builtin_error(msg);
        return NULL;
    }
    if (start >= end) return east_vector_new(vec->data.vector.elem_type, 0);
    size_t count = (size_t)(end - start);
    EastValue *result = east_vector_new(vec->data.vector.elem_type, count);
    size_t es = elem_size(vec->data.vector.elem_type);
    memcpy(result->data.vector.data, (char *)vec->data.vector.data + (size_t)start * es, count * es);
    return result;
}

static EastValue *vector_concat_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *a = args[0];
    EastValue *b = args[1];
    size_t alen = a->data.vector.len;
    size_t blen = b->data.vector.len;
    EastValue *result = east_vector_new(a->data.vector.elem_type, alen + blen);
    size_t es = elem_size(a->data.vector.elem_type);
    memcpy(result->data.vector.data, a->data.vector.data, alen * es);
    memcpy((char *)result->data.vector.data + alen * es, b->data.vector.data, blen * es);
    return result;
}

static EastValue *vector_from_array_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    size_t len = east_array_len(arr);
    /* Determine elem type from type params or from array contents */
    EastType *et = arr->data.array.elem_type ? arr->data.array.elem_type : &east_float_type;
    EastValue *result = east_vector_new(et, len);
    for (size_t i = 0; i < len; i++) {
        vec_set_elem(result, i, east_array_get(arr, i));
    }
    return result;
}

static EastValue *vector_to_array_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *vec = args[0];
    size_t len = vec->data.vector.len;
    EastValue *result = east_array_new(vec->data.vector.elem_type);
    for (size_t i = 0; i < len; i++) {
        EastValue *elem = vec_get_elem(vec, i);
        east_array_push(result, elem);
        east_value_release(elem);
    }
    return result;
}

static EastValue *vector_to_matrix_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *vec = args[0];
    int64_t rows = args[1]->data.integer;
    int64_t cols = args[2]->data.integer;
    if (rows * cols != (int64_t)vec->data.vector.len) return east_null();
    EastValue *mat = east_matrix_new(vec->data.vector.elem_type, (size_t)rows, (size_t)cols);
    size_t es = elem_size(vec->data.vector.elem_type);
    memcpy(mat->data.matrix.data, vec->data.vector.data, vec->data.vector.len * es);
    return mat;
}

static EastValue *vector_zeros_impl(EastValue **args, size_t n) {
    (void)n;
    /* Type param tp[0] gives element type */
    int64_t length = args[0]->data.integer;
    EastValue *result = east_vector_new(&east_float_type, (size_t)length);
    size_t es = elem_size(&east_float_type);
    memset(result->data.vector.data, 0, (size_t)length * es);
    return result;
}

static EastValue *vector_ones_impl(EastValue **args, size_t n) {
    (void)n;
    int64_t length = args[0]->data.integer;
    EastValue *result = east_vector_new(&east_float_type, (size_t)length);
    double *data = (double *)result->data.vector.data;
    for (int64_t i = 0; i < length; i++) data[i] = 1.0;
    return result;
}

static EastValue *vector_fill_impl(EastValue **args, size_t n) {
    (void)n;
    int64_t length = args[0]->data.integer;
    EastValue *val = args[1];
    EastType *et = &east_float_type;
    if (val->kind == EAST_VAL_INTEGER) et = &east_integer_type;
    else if (val->kind == EAST_VAL_BOOLEAN) et = &east_boolean_type;
    EastValue *result = east_vector_new(et, (size_t)length);
    for (int64_t i = 0; i < length; i++) {
        vec_set_elem(result, (size_t)i, val);
    }
    return result;
}

static EastValue *vector_map_with_type(EastValue **args, size_t n, EastType *out_type) {
    (void)n;
    EastValue *vec = args[0];
    EastValue *fn = args[1];
    size_t len = vec->data.vector.len;
    EastValue *result = east_vector_new(out_type, len);
    for (size_t i = 0; i < len; i++) {
        EastValue *elem = vec_get_elem(vec, i);
        EastValue *idx = east_integer((int64_t)i);
        EastValue *call_args[] = { elem, idx };
        EastValue *mapped = call_fn(fn, call_args, 2);
        vec_set_elem(result, i, mapped);
        east_value_release(elem);
        east_value_release(idx);
        east_value_release(mapped);
    }
    return result;
}

static EastValue *vector_map_float(EastValue **args, size_t n) { return vector_map_with_type(args, n, &east_float_type); }
static EastValue *vector_map_int(EastValue **args, size_t n) { return vector_map_with_type(args, n, &east_integer_type); }
static EastValue *vector_map_bool(EastValue **args, size_t n) { return vector_map_with_type(args, n, &east_boolean_type); }

static EastValue *vector_fold_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *vec = args[0];
    EastValue *init = args[1];
    EastValue *fn = args[2];
    size_t len = vec->data.vector.len;
    east_value_retain(init);
    EastValue *acc = init;
    for (size_t i = 0; i < len; i++) {
        EastValue *elem = vec_get_elem(vec, i);
        EastValue *idx = east_integer((int64_t)i);
        EastValue *call_args[] = { acc, elem, idx };
        EastValue *new_acc = call_fn(fn, call_args, 3);
        east_value_release(acc);
        east_value_release(elem);
        east_value_release(idx);
        acc = new_acc;
        east_value_retain(acc);
        east_value_release(new_acc);
    }
    return acc;
}

/* --- typed factory functions that use type params for zeros/ones/fill --- */

static BuiltinImpl vector_zeros_typed_factory(EastType **tp, size_t ntp) {
    (void)ntp;
    /* We store the type param and return a specialized function.
       Since our impl needs the type at call time and we can only return a
       single function pointer, we use the generic impl which defaults to float.
       A more complete implementation would allocate a closure. */
    (void)tp;
    return vector_zeros_impl;
}

static BuiltinImpl vector_ones_typed_factory(EastType **tp, size_t ntp) {
    (void)tp; (void)ntp;
    return vector_ones_impl;
}

static BuiltinImpl vector_fill_typed_factory(EastType **tp, size_t ntp) {
    (void)tp; (void)ntp;
    return vector_fill_impl;
}

/* --- factory functions --- */

static BuiltinImpl vector_length_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return vector_length_impl; }
static BuiltinImpl vector_get_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return vector_get_impl; }
static BuiltinImpl vector_set_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return vector_set_impl; }
static BuiltinImpl vector_slice_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return vector_slice_impl; }
static BuiltinImpl vector_concat_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return vector_concat_impl; }
static BuiltinImpl vector_from_array_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return vector_from_array_impl; }
static BuiltinImpl vector_to_array_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return vector_to_array_impl; }
static BuiltinImpl vector_to_matrix_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return vector_to_matrix_impl; }
static BuiltinImpl vector_map_factory(EastType **tp, size_t ntp) {
    /* tp[0]=input elem, tp[1]=output elem */
    if (ntp >= 2 && tp[1]) {
        if (tp[1]->kind == EAST_TYPE_INTEGER) return vector_map_int;
        if (tp[1]->kind == EAST_TYPE_BOOLEAN) return vector_map_bool;
    }
    return vector_map_float;
}
static BuiltinImpl vector_fold_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return vector_fold_impl; }

/* --- registration --- */

void east_register_vector_builtins(BuiltinRegistry *reg) {
    builtin_registry_register(reg, "VectorLength", vector_length_factory);
    builtin_registry_register(reg, "VectorGet", vector_get_factory);
    builtin_registry_register(reg, "VectorSet", vector_set_factory);
    builtin_registry_register(reg, "VectorSlice", vector_slice_factory);
    builtin_registry_register(reg, "VectorConcat", vector_concat_factory);
    builtin_registry_register(reg, "VectorFromArray", vector_from_array_factory);
    builtin_registry_register(reg, "VectorToArray", vector_to_array_factory);
    builtin_registry_register(reg, "VectorToMatrix", vector_to_matrix_factory);
    builtin_registry_register(reg, "VectorZeros", vector_zeros_typed_factory);
    builtin_registry_register(reg, "VectorOnes", vector_ones_typed_factory);
    builtin_registry_register(reg, "VectorFill", vector_fill_typed_factory);
    builtin_registry_register(reg, "VectorMap", vector_map_factory);
    builtin_registry_register(reg, "VectorFold", vector_fold_factory);
}
