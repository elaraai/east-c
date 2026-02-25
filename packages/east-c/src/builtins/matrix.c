/*
 * Matrix builtin functions.
 *
 * Matrices store homogeneous numeric data in row-major order.
 * data.matrix.data points to a contiguous buffer of (rows * cols) elements.
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
/* Helpers: element access based on type                               */
/* ------------------------------------------------------------------ */
static size_t elem_size(EastType *et) {
    if (et->kind == EAST_TYPE_FLOAT) return sizeof(double);
    if (et->kind == EAST_TYPE_INTEGER) return sizeof(int64_t);
    if (et->kind == EAST_TYPE_BOOLEAN) return sizeof(bool);
    return sizeof(double);
}

static EastValue *mat_get_elem(EastValue *mat, size_t r, size_t c) {
    EastType *et = mat->data.matrix.elem_type;
    size_t cols = mat->data.matrix.cols;
    size_t idx = r * cols + c;
    if (et->kind == EAST_TYPE_FLOAT)
        return east_float(((double *)mat->data.matrix.data)[idx]);
    if (et->kind == EAST_TYPE_INTEGER)
        return east_integer(((int64_t *)mat->data.matrix.data)[idx]);
    if (et->kind == EAST_TYPE_BOOLEAN)
        return east_boolean(((bool *)mat->data.matrix.data)[idx]);
    return east_null();
}

static void mat_set_elem(EastValue *mat, size_t r, size_t c, EastValue *val) {
    EastType *et = mat->data.matrix.elem_type;
    size_t cols = mat->data.matrix.cols;
    size_t idx = r * cols + c;
    if (et->kind == EAST_TYPE_FLOAT)
        ((double *)mat->data.matrix.data)[idx] = val->data.float64;
    else if (et->kind == EAST_TYPE_INTEGER)
        ((int64_t *)mat->data.matrix.data)[idx] = val->data.integer;
    else if (et->kind == EAST_TYPE_BOOLEAN)
        ((bool *)mat->data.matrix.data)[idx] = val->data.boolean;
}

/* ------------------------------------------------------------------ */
/* Vector element helpers (reused from vector.c logic)                */
/* ------------------------------------------------------------------ */
static EastValue *vec_get_elem(EastValue *vec, size_t i) {
    EastType *et = vec->data.vector.elem_type;
    if (et->kind == EAST_TYPE_FLOAT)
        return east_float(((double *)vec->data.vector.data)[i]);
    if (et->kind == EAST_TYPE_INTEGER)
        return east_integer(((int64_t *)vec->data.vector.data)[i]);
    if (et->kind == EAST_TYPE_BOOLEAN)
        return east_boolean(((bool *)vec->data.vector.data)[i]);
    return east_null();
}

/* --- implementations --- */

static EastValue *matrix_rows_impl(EastValue **args, size_t n) {
    (void)n;
    return east_integer((int64_t)args[0]->data.matrix.rows);
}

static EastValue *matrix_cols_impl(EastValue **args, size_t n) {
    (void)n;
    return east_integer((int64_t)args[0]->data.matrix.cols);
}

static EastValue *matrix_get_impl(EastValue **args, size_t n) {
    (void)n;
    int64_t row = args[1]->data.integer;
    int64_t col = args[2]->data.integer;
    EastValue *mat = args[0];
    if (row < 0 || (size_t)row >= mat->data.matrix.rows ||
        col < 0 || (size_t)col >= mat->data.matrix.cols) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Matrix index (%lld, %lld) out of bounds (%zux%zu)",
                 (long long)row, (long long)col,
                 mat->data.matrix.rows, mat->data.matrix.cols);
        east_builtin_error(msg);
        return NULL;
    }
    return mat_get_elem(mat, (size_t)row, (size_t)col);
}

static EastValue *matrix_set_impl(EastValue **args, size_t n) {
    (void)n;
    int64_t row = args[1]->data.integer;
    int64_t col = args[2]->data.integer;
    EastValue *mat = args[0];
    if (row < 0 || (size_t)row >= mat->data.matrix.rows ||
        col < 0 || (size_t)col >= mat->data.matrix.cols) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Matrix index (%lld, %lld) out of bounds (%zux%zu)",
                 (long long)row, (long long)col,
                 mat->data.matrix.rows, mat->data.matrix.cols);
        east_builtin_error(msg);
        return NULL;
    }
    mat_set_elem(mat, (size_t)row, (size_t)col, args[3]);
    return east_null();
}

static EastValue *matrix_get_row_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *mat = args[0];
    int64_t row = args[1]->data.integer;
    if (row < 0 || (size_t)row >= mat->data.matrix.rows) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Matrix row %lld out of bounds (%zu rows)",
                 (long long)row, mat->data.matrix.rows);
        east_builtin_error(msg);
        return NULL;
    }
    size_t cols = mat->data.matrix.cols;
    EastValue *vec = east_vector_new(mat->data.matrix.elem_type, cols);
    size_t es = elem_size(mat->data.matrix.elem_type);
    memcpy(vec->data.vector.data, (char *)mat->data.matrix.data + (size_t)row * cols * es, cols * es);
    return vec;
}

static EastValue *matrix_get_col_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *mat = args[0];
    int64_t col = args[1]->data.integer;
    if (col < 0 || (size_t)col >= mat->data.matrix.cols) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Matrix column %lld out of bounds (%zu cols)",
                 (long long)col, mat->data.matrix.cols);
        east_builtin_error(msg);
        return NULL;
    }
    size_t rows = mat->data.matrix.rows;
    size_t cols = mat->data.matrix.cols;
    EastValue *vec = east_vector_new(mat->data.matrix.elem_type, rows);
    EastType *et = mat->data.matrix.elem_type;
    for (size_t r = 0; r < rows; r++) {
        size_t src_idx = r * cols + (size_t)col;
        if (et->kind == EAST_TYPE_FLOAT)
            ((double *)vec->data.vector.data)[r] = ((double *)mat->data.matrix.data)[src_idx];
        else if (et->kind == EAST_TYPE_INTEGER)
            ((int64_t *)vec->data.vector.data)[r] = ((int64_t *)mat->data.matrix.data)[src_idx];
        else if (et->kind == EAST_TYPE_BOOLEAN)
            ((bool *)vec->data.vector.data)[r] = ((bool *)mat->data.matrix.data)[src_idx];
    }
    return vec;
}

static EastValue *matrix_to_vector_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *mat = args[0];
    size_t total = mat->data.matrix.rows * mat->data.matrix.cols;
    EastValue *vec = east_vector_new(mat->data.matrix.elem_type, total);
    size_t es = elem_size(mat->data.matrix.elem_type);
    memcpy(vec->data.vector.data, mat->data.matrix.data, total * es);
    return vec;
}

static EastValue *matrix_from_array_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    size_t rows = east_array_len(arr);
    if (rows == 0) return east_matrix_new(&east_float_type, 0, 0);
    EastValue *first_row = east_array_get(arr, 0);
    size_t cols = east_array_len(first_row);
    /* Determine element type from first element */
    EastType *et = &east_float_type;
    if (cols > 0) {
        EastValue *first_elem = east_array_get(first_row, 0);
        if (first_elem->kind == EAST_VAL_INTEGER) et = &east_integer_type;
        else if (first_elem->kind == EAST_VAL_BOOLEAN) et = &east_boolean_type;
    }
    EastValue *mat = east_matrix_new(et, rows, cols);
    for (size_t r = 0; r < rows; r++) {
        EastValue *row = east_array_get(arr, r);
        for (size_t c = 0; c < cols; c++) {
            mat_set_elem(mat, r, c, east_array_get(row, c));
        }
    }
    return mat;
}

static EastValue *matrix_to_array_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *mat = args[0];
    size_t rows = mat->data.matrix.rows;
    size_t cols = mat->data.matrix.cols;
    EastType *et = mat->data.matrix.elem_type;
    EastType *arr_et = east_array_type(et);
    EastValue *result = east_array_new(arr_et);
    for (size_t r = 0; r < rows; r++) {
        EastValue *row = east_array_new(et);
        for (size_t c = 0; c < cols; c++) {
            EastValue *elem = mat_get_elem(mat, r, c);
            east_array_push(row, elem);
            east_value_release(elem);
        }
        east_array_push(result, row);
        east_value_release(row);
    }
    east_type_release(arr_et);
    return result;
}

static EastValue *matrix_transpose_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *mat = args[0];
    size_t rows = mat->data.matrix.rows;
    size_t cols = mat->data.matrix.cols;
    EastValue *result = east_matrix_new(mat->data.matrix.elem_type, cols, rows);
    EastType *et = mat->data.matrix.elem_type;
    size_t es = elem_size(et);
    for (size_t r = 0; r < rows; r++) {
        for (size_t c = 0; c < cols; c++) {
            size_t src = r * cols + c;
            size_t dst = c * rows + r;
            memcpy((char *)result->data.matrix.data + dst * es,
                   (char *)mat->data.matrix.data + src * es, es);
        }
    }
    return result;
}

static EastValue *matrix_zeros_impl(EastValue **args, size_t n) {
    (void)n;
    size_t rows = (size_t)args[0]->data.integer;
    size_t cols = (size_t)args[1]->data.integer;
    EastValue *mat = east_matrix_new(&east_float_type, rows, cols);
    memset(mat->data.matrix.data, 0, rows * cols * sizeof(double));
    return mat;
}

static EastValue *matrix_ones_impl(EastValue **args, size_t n) {
    (void)n;
    size_t rows = (size_t)args[0]->data.integer;
    size_t cols = (size_t)args[1]->data.integer;
    EastValue *mat = east_matrix_new(&east_float_type, rows, cols);
    double *data = (double *)mat->data.matrix.data;
    for (size_t i = 0; i < rows * cols; i++) data[i] = 1.0;
    return mat;
}

static EastValue *matrix_fill_impl(EastValue **args, size_t n) {
    (void)n;
    size_t rows = (size_t)args[0]->data.integer;
    size_t cols = (size_t)args[1]->data.integer;
    EastValue *val = args[2];
    EastType *et = &east_float_type;
    if (val->kind == EAST_VAL_INTEGER) et = &east_integer_type;
    else if (val->kind == EAST_VAL_BOOLEAN) et = &east_boolean_type;
    EastValue *mat = east_matrix_new(et, rows, cols);
    for (size_t r = 0; r < rows; r++)
        for (size_t c = 0; c < cols; c++)
            mat_set_elem(mat, r, c, val);
    return mat;
}

static EastValue *matrix_map_elements_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *mat = args[0];
    EastValue *fn = args[1];
    size_t rows = mat->data.matrix.rows;
    size_t cols = mat->data.matrix.cols;
    EastValue *result = east_matrix_new(mat->data.matrix.elem_type, rows, cols);
    for (size_t r = 0; r < rows; r++) {
        for (size_t c = 0; c < cols; c++) {
            EastValue *elem = mat_get_elem(mat, r, c);
            EastValue *ri = east_integer((int64_t)r);
            EastValue *ci = east_integer((int64_t)c);
            EastValue *call_args[] = { elem, ri, ci };
            EastValue *mapped = call_fn(fn, call_args, 3);
            mat_set_elem(result, r, c, mapped);
            east_value_release(elem);
            east_value_release(ri);
            east_value_release(ci);
            east_value_release(mapped);
        }
    }
    return result;
}

static EastValue *matrix_map_rows_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *mat = args[0];
    EastValue *fn = args[1];
    size_t rows = mat->data.matrix.rows;
    size_t cols = mat->data.matrix.cols;
    EastType *et = mat->data.matrix.elem_type;
    size_t es = elem_size(et);

    /* Collect result row vectors */
    EastValue **row_vecs = malloc(rows * sizeof(EastValue *));
    size_t result_cols = 0;
    for (size_t r = 0; r < rows; r++) {
        EastValue *row_vec = east_vector_new(et, cols);
        memcpy(row_vec->data.vector.data, (char *)mat->data.matrix.data + r * cols * es, cols * es);
        EastValue *ri = east_integer((int64_t)r);
        EastValue *call_args[] = { row_vec, ri };
        EastValue *result_vec = call_fn(fn, call_args, 2);
        row_vecs[r] = result_vec;
        if (r == 0) result_cols = result_vec->data.vector.len;
        east_value_release(row_vec);
        east_value_release(ri);
    }

    EastType *ret = (rows > 0 && row_vecs[0]) ? row_vecs[0]->data.vector.elem_type : et;
    EastValue *result = east_matrix_new(ret, rows, result_cols);
    size_t res = elem_size(ret);
    for (size_t r = 0; r < rows; r++) {
        memcpy((char *)result->data.matrix.data + r * result_cols * res,
               row_vecs[r]->data.vector.data, result_cols * res);
        east_value_release(row_vecs[r]);
    }
    free(row_vecs);
    return result;
}

static EastValue *matrix_to_rows_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *mat = args[0];
    size_t rows = mat->data.matrix.rows;
    size_t cols = mat->data.matrix.cols;
    EastType *et = mat->data.matrix.elem_type;
    size_t es = elem_size(et);
    EastType *vec_t = east_vector_type(et);
    EastValue *result = east_array_new(vec_t);
    for (size_t r = 0; r < rows; r++) {
        EastValue *row_vec = east_vector_new(et, cols);
        memcpy(row_vec->data.vector.data, (char *)mat->data.matrix.data + r * cols * es, cols * es);
        east_array_push(result, row_vec);
        east_value_release(row_vec);
    }
    east_type_release(vec_t);
    return result;
}

static EastValue *matrix_from_rows_impl(EastValue **args, size_t n) {
    (void)n;
    EastValue *arr = args[0];
    size_t rows = east_array_len(arr);
    if (rows == 0) return east_matrix_new(&east_float_type, 0, 0);
    EastValue *first = east_array_get(arr, 0);
    size_t cols = first->data.vector.len;
    EastType *et = first->data.vector.elem_type;
    size_t es = elem_size(et);
    EastValue *mat = east_matrix_new(et, rows, cols);
    for (size_t r = 0; r < rows; r++) {
        EastValue *rv = east_array_get(arr, r);
        memcpy((char *)mat->data.matrix.data + r * cols * es, rv->data.vector.data, cols * es);
    }
    return mat;
}

/* --- factory functions --- */

static BuiltinImpl matrix_rows_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return matrix_rows_impl; }
static BuiltinImpl matrix_cols_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return matrix_cols_impl; }
static BuiltinImpl matrix_get_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return matrix_get_impl; }
static BuiltinImpl matrix_set_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return matrix_set_impl; }
static BuiltinImpl matrix_get_row_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return matrix_get_row_impl; }
static BuiltinImpl matrix_get_col_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return matrix_get_col_impl; }
static BuiltinImpl matrix_to_vector_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return matrix_to_vector_impl; }
static BuiltinImpl matrix_from_array_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return matrix_from_array_impl; }
static BuiltinImpl matrix_to_array_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return matrix_to_array_impl; }
static BuiltinImpl matrix_transpose_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return matrix_transpose_impl; }
static BuiltinImpl matrix_zeros_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return matrix_zeros_impl; }
static BuiltinImpl matrix_ones_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return matrix_ones_impl; }
static BuiltinImpl matrix_fill_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return matrix_fill_impl; }
static BuiltinImpl matrix_map_elements_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return matrix_map_elements_impl; }
static BuiltinImpl matrix_map_rows_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return matrix_map_rows_impl; }
static BuiltinImpl matrix_to_rows_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return matrix_to_rows_impl; }
static BuiltinImpl matrix_from_rows_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return matrix_from_rows_impl; }

/* --- registration --- */

void east_register_matrix_builtins(BuiltinRegistry *reg) {
    builtin_registry_register(reg, "MatrixRows", matrix_rows_factory);
    builtin_registry_register(reg, "MatrixCols", matrix_cols_factory);
    builtin_registry_register(reg, "MatrixGet", matrix_get_factory);
    builtin_registry_register(reg, "MatrixSet", matrix_set_factory);
    builtin_registry_register(reg, "MatrixGetRow", matrix_get_row_factory);
    builtin_registry_register(reg, "MatrixGetCol", matrix_get_col_factory);
    builtin_registry_register(reg, "MatrixToVector", matrix_to_vector_factory);
    builtin_registry_register(reg, "MatrixFromArray", matrix_from_array_factory);
    builtin_registry_register(reg, "MatrixToArray", matrix_to_array_factory);
    builtin_registry_register(reg, "MatrixTranspose", matrix_transpose_factory);
    builtin_registry_register(reg, "MatrixZeros", matrix_zeros_factory);
    builtin_registry_register(reg, "MatrixOnes", matrix_ones_factory);
    builtin_registry_register(reg, "MatrixFill", matrix_fill_factory);
    builtin_registry_register(reg, "MatrixMapElements", matrix_map_elements_factory);
    builtin_registry_register(reg, "MatrixMapRows", matrix_map_rows_factory);
    builtin_registry_register(reg, "MatrixToRows", matrix_to_rows_factory);
    builtin_registry_register(reg, "MatrixFromRows", matrix_from_rows_factory);
}
