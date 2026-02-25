#include "east/values.h"
#include "east/arena.h"
#include "east/gc.h"
#include "east/types.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Global null singleton                                              */
/* ------------------------------------------------------------------ */

EastValue east_null_value = { .kind = EAST_VAL_NULL, .ref_count = -1 };

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static bool is_gc_type(EastValueKind kind) {
    switch (kind) {
    case EAST_VAL_ARRAY:
    case EAST_VAL_SET:
    case EAST_VAL_DICT:
    case EAST_VAL_STRUCT:
    case EAST_VAL_VARIANT:
    case EAST_VAL_REF:
    case EAST_VAL_FUNCTION:
        return true;
    default:
        return false;
    }
}

static EastValue *alloc_value(EastValueKind kind) {
    EastValue *v = east_calloc(1, sizeof(EastValue));
    if (!v) return NULL;
    v->kind = kind;
    v->ref_count = 1;
    v->gc_tracked = false;
    v->gc_next = NULL;
    v->gc_prev = NULL;
    if (is_gc_type(kind)) {
        east_gc_track(v);
    }
    return v;
}

/*
 * Format a double identically to ECMAScript Number::toString(x).
 * Implements the algorithm from ECMA-262 section 6.1.6.1.20 exactly:
 *   1. NaN → "NaN"
 *   2. +0 or -0 → "0"
 *   3. Negative → "-" + ToString(-x)
 *   4. Infinity → "Infinity"
 *   5. Find minimal k significant digits, then format per spec rules.
 */
int east_fmt_double(char *out, size_t out_size, double val) {
    if (isnan(val)) return snprintf(out, out_size, "NaN");
    if (val == 0.0) return snprintf(out, out_size, "0"); /* both +0 and -0 */
    if (isinf(val)) return snprintf(out, out_size, val > 0 ? "Infinity" : "-Infinity");

    int pos = 0;
    if (val < 0) { out[pos++] = '-'; val = -val; }

    /* Find shortest %e representation that round-trips */
    char ebuf[32];
    int prec;
    for (prec = 0; prec <= 20; prec++) {
        snprintf(ebuf, sizeof(ebuf), "%.*e", prec, val);
        if (strtod(ebuf, NULL) == val) break;
    }

    /* Parse significant digits and exponent from %e output (e.g. "3.14e+02") */
    char digits[24];
    int k = 0;
    char *p = ebuf;
    digits[k++] = *p++;
    if (*p == '.') {
        p++;
        while (*p && *p != 'e' && *p != 'E')
            digits[k++] = *p++;
    }
    digits[k] = '\0';

    /* Strip trailing zeros from digit string */
    while (k > 1 && digits[k - 1] == '0') k--;
    digits[k] = '\0';

    int exp_val = 0;
    if (*p == 'e' || *p == 'E') { p++; exp_val = atoi(p); }
    int n = exp_val + 1; /* ECMAScript "n" */

    /* ECMAScript formatting rules (steps 6-10 of 9.8.1) */
    if (k <= n && n <= 21) {
        /* digits followed by (n-k) zeros */
        for (int i = 0; i < k; i++) out[pos++] = digits[i];
        for (int i = 0; i < n - k; i++) out[pos++] = '0';
        out[pos] = '\0';
    } else if (0 < n && n <= 21) {
        /* first n digits, '.', remaining digits */
        for (int i = 0; i < n; i++) out[pos++] = digits[i];
        out[pos++] = '.';
        for (int i = n; i < k; i++) out[pos++] = digits[i];
        out[pos] = '\0';
    } else if (-6 < n && n <= 0) {
        /* "0.", (-n) zeros, then digits */
        out[pos++] = '0'; out[pos++] = '.';
        for (int i = 0; i < -n; i++) out[pos++] = '0';
        for (int i = 0; i < k; i++) out[pos++] = digits[i];
        out[pos] = '\0';
    } else {
        /* Scientific notation */
        int e = n - 1;
        out[pos++] = digits[0];
        if (k > 1) {
            out[pos++] = '.';
            for (int i = 1; i < k; i++) out[pos++] = digits[i];
        }
        pos += snprintf(out + pos, out_size - pos, "e%c%d",
                        e >= 0 ? '+' : '-', e >= 0 ? e : -e);
    }
    return pos;
}

/* Return the byte size of a single element for vector/matrix storage. */
static size_t elem_size_for_type(EastType *elem_type) {
    if (!elem_type) return sizeof(double);
    switch (elem_type->kind) {
    case EAST_TYPE_FLOAT:   return sizeof(double);
    case EAST_TYPE_INTEGER: return sizeof(int64_t);
    case EAST_TYPE_BOOLEAN: return sizeof(bool);
    default:                return sizeof(double);
    }
}

/* ------------------------------------------------------------------ */
/*  Constructors: primitives                                           */
/* ------------------------------------------------------------------ */

EastValue *east_null(void) {
    return &east_null_value;
}

EastValue *east_boolean(bool val) {
    EastValue *v = alloc_value(EAST_VAL_BOOLEAN);
    if (!v) return NULL;
    v->data.boolean = val;
    return v;
}

EastValue *east_integer(int64_t val) {
    EastValue *v = alloc_value(EAST_VAL_INTEGER);
    if (!v) return NULL;
    v->data.integer = val;
    return v;
}

EastValue *east_float(double val) {
    EastValue *v = alloc_value(EAST_VAL_FLOAT);
    if (!v) return NULL;
    v->data.float64 = val;
    return v;
}

EastValue *east_string(const char *str) {
    if (!str) str = "";
    EastValue *v = alloc_value(EAST_VAL_STRING);
    if (!v) return NULL;
    v->data.string.len = strlen(str);
    v->data.string.data = east_strdup(str);
    if (!v->data.string.data) {
        east_free(v);
        return NULL;
    }
    return v;
}

EastValue *east_string_len(const char *str, size_t len) {
    EastValue *v = alloc_value(EAST_VAL_STRING);
    if (!v) return NULL;
    v->data.string.len = len;
    v->data.string.data = east_alloc(len + 1);
    if (!v->data.string.data) {
        east_free(v);
        return NULL;
    }
    if (str && len > 0) {
        memcpy(v->data.string.data, str, len);
    }
    v->data.string.data[len] = '\0';
    return v;
}

EastValue *east_datetime(int64_t millis) {
    EastValue *v = alloc_value(EAST_VAL_DATETIME);
    if (!v) return NULL;
    v->data.datetime = millis;
    return v;
}

EastValue *east_blob(const uint8_t *data, size_t len) {
    EastValue *v = alloc_value(EAST_VAL_BLOB);
    if (!v) return NULL;
    v->data.blob.len = len;
    if (len > 0 && data) {
        v->data.blob.data = east_alloc(len);
        if (!v->data.blob.data) {
            east_free(v);
            return NULL;
        }
        memcpy(v->data.blob.data, data, len);
    } else {
        v->data.blob.data = NULL;
    }
    return v;
}

/* ------------------------------------------------------------------ */
/*  Constructors: collections                                          */
/* ------------------------------------------------------------------ */

EastValue *east_array_new(EastType *elem_type) {
    EastValue *v = alloc_value(EAST_VAL_ARRAY);
    if (!v) return NULL;
    v->data.array.len = 0;
    v->data.array.cap = 4;
    v->data.array.items = east_alloc(4 * sizeof(EastValue *));
    if (!v->data.array.items) {
        east_free(v);
        return NULL;
    }
    v->data.array.elem_type = elem_type;
    if (elem_type) east_type_retain(elem_type);
    return v;
}

void east_array_push(EastValue *arr, EastValue *val) {
    if (!arr || arr->kind != EAST_VAL_ARRAY) return;
    if (arr->data.array.len >= arr->data.array.cap) {
        size_t old_cap = arr->data.array.cap;
        size_t new_cap = old_cap * 2;
        EastValue **new_items = east_realloc(arr->data.array.items,
                                             old_cap * sizeof(EastValue *),
                                             new_cap * sizeof(EastValue *));
        if (!new_items) return;
        arr->data.array.items = new_items;
        arr->data.array.cap = new_cap;
    }
    if (val) east_value_retain(val);
    arr->data.array.items[arr->data.array.len++] = val;
}

EastValue *east_array_get(EastValue *arr, size_t index) {
    if (!arr || arr->kind != EAST_VAL_ARRAY) return NULL;
    if (index >= arr->data.array.len) return NULL;
    return arr->data.array.items[index];
}

size_t east_array_len(EastValue *arr) {
    if (!arr || arr->kind != EAST_VAL_ARRAY) return 0;
    return arr->data.array.len;
}

/* ------------------------------------------------------------------ */
/*  Sorted set                                                         */
/* ------------------------------------------------------------------ */

EastValue *east_set_new(EastType *elem_type) {
    EastValue *v = alloc_value(EAST_VAL_SET);
    if (!v) return NULL;
    v->data.set.len = 0;
    v->data.set.cap = 4;
    v->data.set.items = east_alloc(4 * sizeof(EastValue *));
    if (!v->data.set.items) {
        east_free(v);
        return NULL;
    }
    v->data.set.elem_type = elem_type;
    if (elem_type) east_type_retain(elem_type);
    return v;
}

/*
 * Binary search in sorted items array.
 * Returns the index where `val` was found, or the insertion point if not found.
 * Sets *found to true if the value is already present.
 */
static size_t sorted_search(EastValue **items, size_t len, EastValue *val,
                            bool *found) {
    size_t lo = 0;
    size_t hi = len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = east_value_compare(items[mid], val);
        if (cmp < 0) {
            lo = mid + 1;
        } else if (cmp > 0) {
            hi = mid;
        } else {
            *found = true;
            return mid;
        }
    }
    *found = false;
    return lo;
}

void east_set_insert(EastValue *set, EastValue *val) {
    if (!set || set->kind != EAST_VAL_SET) return;

    bool found = false;
    size_t pos = sorted_search(set->data.set.items, set->data.set.len, val,
                               &found);
    if (found) return; /* already present */

    /* Grow if needed. */
    if (set->data.set.len >= set->data.set.cap) {
        size_t old_cap = set->data.set.cap;
        size_t new_cap = old_cap * 2;
        EastValue **new_items = east_realloc(set->data.set.items,
                                             old_cap * sizeof(EastValue *),
                                             new_cap * sizeof(EastValue *));
        if (!new_items) return;
        set->data.set.items = new_items;
        set->data.set.cap = new_cap;
    }

    /* Shift elements right to make room. */
    if (pos < set->data.set.len) {
        memmove(&set->data.set.items[pos + 1],
                &set->data.set.items[pos],
                (set->data.set.len - pos) * sizeof(EastValue *));
    }

    if (val) east_value_retain(val);
    set->data.set.items[pos] = val;
    set->data.set.len++;
}

bool east_set_has(EastValue *set, EastValue *val) {
    if (!set || set->kind != EAST_VAL_SET) return false;
    bool found = false;
    sorted_search(set->data.set.items, set->data.set.len, val, &found);
    return found;
}

bool east_set_delete(EastValue *set, EastValue *val) {
    if (!set || set->kind != EAST_VAL_SET) return false;
    bool found = false;
    size_t pos = sorted_search(set->data.set.items, set->data.set.len, val, &found);
    if (!found) return false;
    east_value_release(set->data.set.items[pos]);
    size_t remaining = set->data.set.len - pos - 1;
    if (remaining > 0) {
        memmove(&set->data.set.items[pos], &set->data.set.items[pos + 1],
                remaining * sizeof(EastValue *));
    }
    set->data.set.len--;
    return true;
}

size_t east_set_len(EastValue *set) {
    if (!set || set->kind != EAST_VAL_SET) return 0;
    return set->data.set.len;
}

/* ------------------------------------------------------------------ */
/*  Sorted dict (parallel arrays)                                      */
/* ------------------------------------------------------------------ */

EastValue *east_dict_new(EastType *key_type, EastType *val_type) {
    EastValue *v = alloc_value(EAST_VAL_DICT);
    if (!v) return NULL;
    v->data.dict.len = 0;
    v->data.dict.cap = 4;
    v->data.dict.keys = east_alloc(4 * sizeof(EastValue *));
    v->data.dict.values = east_alloc(4 * sizeof(EastValue *));
    if (!v->data.dict.keys || !v->data.dict.values) {
        east_free(v->data.dict.keys);
        east_free(v->data.dict.values);
        east_free(v);
        return NULL;
    }
    v->data.dict.key_type = key_type;
    if (key_type) east_type_retain(key_type);
    v->data.dict.val_type = val_type;
    if (val_type) east_type_retain(val_type);
    return v;
}

void east_dict_set(EastValue *dict, EastValue *key, EastValue *val) {
    if (!dict || dict->kind != EAST_VAL_DICT) return;

    bool found = false;
    size_t pos = sorted_search(dict->data.dict.keys, dict->data.dict.len, key,
                               &found);

    if (found) {
        /* Update existing entry. */
        if (val) east_value_retain(val);
        east_value_release(dict->data.dict.values[pos]);
        dict->data.dict.values[pos] = val;
        return;
    }

    /* Grow if needed. */
    if (dict->data.dict.len >= dict->data.dict.cap) {
        size_t old_cap = dict->data.dict.cap;
        size_t new_cap = old_cap * 2;
        EastValue **new_keys = east_realloc(dict->data.dict.keys,
                                            old_cap * sizeof(EastValue *),
                                            new_cap * sizeof(EastValue *));
        EastValue **new_vals = east_realloc(dict->data.dict.values,
                                            old_cap * sizeof(EastValue *),
                                            new_cap * sizeof(EastValue *));
        if (!new_keys || !new_vals) {
            /* On partial realloc failure, restore what we can. */
            if (new_keys) dict->data.dict.keys = new_keys;
            if (new_vals) dict->data.dict.values = new_vals;
            return;
        }
        dict->data.dict.keys = new_keys;
        dict->data.dict.values = new_vals;
        dict->data.dict.cap = new_cap;
    }

    /* Shift elements right. */
    if (pos < dict->data.dict.len) {
        memmove(&dict->data.dict.keys[pos + 1],
                &dict->data.dict.keys[pos],
                (dict->data.dict.len - pos) * sizeof(EastValue *));
        memmove(&dict->data.dict.values[pos + 1],
                &dict->data.dict.values[pos],
                (dict->data.dict.len - pos) * sizeof(EastValue *));
    }

    if (key) east_value_retain(key);
    if (val) east_value_retain(val);
    dict->data.dict.keys[pos] = key;
    dict->data.dict.values[pos] = val;
    dict->data.dict.len++;
}

EastValue *east_dict_get(EastValue *dict, EastValue *key) {
    if (!dict || dict->kind != EAST_VAL_DICT) return NULL;
    bool found = false;
    size_t pos = sorted_search(dict->data.dict.keys, dict->data.dict.len, key,
                               &found);
    if (!found) return NULL;
    return dict->data.dict.values[pos];
}

bool east_dict_has(EastValue *dict, EastValue *key) {
    if (!dict || dict->kind != EAST_VAL_DICT) return false;
    bool found = false;
    sorted_search(dict->data.dict.keys, dict->data.dict.len, key, &found);
    return found;
}

bool east_dict_delete(EastValue *dict, EastValue *key) {
    if (!dict || dict->kind != EAST_VAL_DICT) return false;
    bool found = false;
    size_t pos = sorted_search(dict->data.dict.keys, dict->data.dict.len, key, &found);
    if (!found) return false;
    east_value_release(dict->data.dict.keys[pos]);
    east_value_release(dict->data.dict.values[pos]);
    size_t remaining = dict->data.dict.len - pos - 1;
    if (remaining > 0) {
        memmove(&dict->data.dict.keys[pos], &dict->data.dict.keys[pos + 1],
                remaining * sizeof(EastValue *));
        memmove(&dict->data.dict.values[pos], &dict->data.dict.values[pos + 1],
                remaining * sizeof(EastValue *));
    }
    dict->data.dict.len--;
    return true;
}

EastValue *east_dict_pop(EastValue *dict, EastValue *key) {
    if (!dict || dict->kind != EAST_VAL_DICT) return NULL;
    bool found = false;
    size_t pos = sorted_search(dict->data.dict.keys, dict->data.dict.len, key, &found);
    if (!found) return NULL;
    EastValue *val = dict->data.dict.values[pos];  /* transfer ownership */
    east_value_release(dict->data.dict.keys[pos]);
    size_t remaining = dict->data.dict.len - pos - 1;
    if (remaining > 0) {
        memmove(&dict->data.dict.keys[pos], &dict->data.dict.keys[pos + 1],
                remaining * sizeof(EastValue *));
        memmove(&dict->data.dict.values[pos], &dict->data.dict.values[pos + 1],
                remaining * sizeof(EastValue *));
    }
    dict->data.dict.len--;
    return val;
}

size_t east_dict_len(EastValue *dict) {
    if (!dict || dict->kind != EAST_VAL_DICT) return 0;
    return dict->data.dict.len;
}

/* ------------------------------------------------------------------ */
/*  Struct / Variant / Ref                                             */
/* ------------------------------------------------------------------ */

EastValue *east_struct_new(const char **names, EastValue **values,
                           size_t count, EastType *type) {
    EastValue *v = alloc_value(EAST_VAL_STRUCT);
    if (!v) return NULL;
    v->data.struct_.num_fields = count;
    v->data.struct_.field_names = NULL;
    v->data.struct_.field_values = NULL;

    if (count > 0) {
        v->data.struct_.field_names = east_alloc(count * sizeof(char *));
        v->data.struct_.field_values = east_alloc(count * sizeof(EastValue *));
        if (!v->data.struct_.field_names || !v->data.struct_.field_values) {
            east_free(v->data.struct_.field_names);
            east_free(v->data.struct_.field_values);
            east_free(v);
            return NULL;
        }
        for (size_t i = 0; i < count; i++) {
            v->data.struct_.field_names[i] = east_strdup(names[i]);
            v->data.struct_.field_values[i] = values[i];
            if (values[i]) east_value_retain(values[i]);
        }
    }

    v->data.struct_.type = type;
    if (type) east_type_retain(type);
    return v;
}

EastValue *east_struct_get_field(EastValue *s, const char *name) {
    if (!s || s->kind != EAST_VAL_STRUCT || !name) return NULL;
    for (size_t i = 0; i < s->data.struct_.num_fields; i++) {
        if (strcmp(s->data.struct_.field_names[i], name) == 0) {
            return s->data.struct_.field_values[i];
        }
    }
    return NULL;
}

EastValue *east_variant_new(const char *case_name, EastValue *value,
                            EastType *type) {
    EastValue *v = alloc_value(EAST_VAL_VARIANT);
    if (!v) return NULL;
    v->data.variant.case_name = east_strdup(case_name ? case_name : "");
    if (!v->data.variant.case_name) {
        east_free(v);
        return NULL;
    }
    v->data.variant.value = value;
    if (value) east_value_retain(value);
    v->data.variant.type = type;
    if (type) east_type_retain(type);
    return v;
}

EastValue *east_ref_new(EastValue *value) {
    EastValue *v = alloc_value(EAST_VAL_REF);
    if (!v) return NULL;
    v->data.ref.value = value;
    if (value) east_value_retain(value);
    return v;
}

EastValue *east_ref_get(EastValue *ref) {
    if (!ref || ref->kind != EAST_VAL_REF) return NULL;
    return ref->data.ref.value;
}

void east_ref_set(EastValue *ref, EastValue *value) {
    if (!ref || ref->kind != EAST_VAL_REF) return;
    if (value) east_value_retain(value);
    east_value_release(ref->data.ref.value);
    ref->data.ref.value = value;
}

/* ------------------------------------------------------------------ */
/*  Vector / Matrix                                                    */
/* ------------------------------------------------------------------ */

EastValue *east_vector_new(EastType *elem_type, size_t len) {
    EastValue *v = alloc_value(EAST_VAL_VECTOR);
    if (!v) return NULL;
    v->data.vector.len = len;
    v->data.vector.elem_type = elem_type;
    if (elem_type) east_type_retain(elem_type);
    size_t esize = elem_size_for_type(elem_type);
    if (len > 0) {
        v->data.vector.data = east_calloc(len, esize);
        if (!v->data.vector.data) {
            if (elem_type) east_type_release(elem_type);
            east_free(v);
            return NULL;
        }
    } else {
        v->data.vector.data = NULL;
    }
    return v;
}

EastValue *east_matrix_new(EastType *elem_type, size_t rows, size_t cols) {
    EastValue *v = alloc_value(EAST_VAL_MATRIX);
    if (!v) return NULL;
    v->data.matrix.rows = rows;
    v->data.matrix.cols = cols;
    v->data.matrix.elem_type = elem_type;
    if (elem_type) east_type_retain(elem_type);
    size_t count = rows * cols;
    size_t esize = elem_size_for_type(elem_type);
    if (count > 0) {
        v->data.matrix.data = east_calloc(count, esize);
        if (!v->data.matrix.data) {
            if (elem_type) east_type_release(elem_type);
            east_free(v);
            return NULL;
        }
    } else {
        v->data.matrix.data = NULL;
    }
    return v;
}

/* ------------------------------------------------------------------ */
/*  Function                                                           */
/* ------------------------------------------------------------------ */

EastValue *east_function_value(EastCompiledFn *fn) {
    EastValue *v = alloc_value(EAST_VAL_FUNCTION);
    if (!v) return NULL;
    v->data.function.compiled = fn;
    return v;
}

/* ------------------------------------------------------------------ */
/*  Ref counting                                                       */
/* ------------------------------------------------------------------ */

void east_value_retain(EastValue *v) {
    if (!v) return;
    if (v->ref_count < 0) return; /* singleton (null) */
    v->ref_count++;
}

void east_value_release(EastValue *v) {
    if (!v) return;
    if (v->ref_count < 0) return; /* singleton (null) */
    v->ref_count--;
    if (v->ref_count > 0) return;

    /* Remove from GC tracking list before freeing. */
    if (v->gc_tracked) east_gc_untrack(v);

    /* ref_count == 0: free resources. */
    switch (v->kind) {
    case EAST_VAL_NULL:
    case EAST_VAL_BOOLEAN:
    case EAST_VAL_INTEGER:
    case EAST_VAL_FLOAT:
    case EAST_VAL_DATETIME:
        /* No heap allocations inside data. */
        break;

    case EAST_VAL_STRING:
        free(v->data.string.data);
        break;

    case EAST_VAL_BLOB:
        free(v->data.blob.data);
        break;

    case EAST_VAL_ARRAY:
        for (size_t i = 0; i < v->data.array.len; i++) {
            east_value_release(v->data.array.items[i]);
        }
        free(v->data.array.items);
        if (v->data.array.elem_type)
            east_type_release(v->data.array.elem_type);
        break;

    case EAST_VAL_SET:
        for (size_t i = 0; i < v->data.set.len; i++) {
            east_value_release(v->data.set.items[i]);
        }
        free(v->data.set.items);
        if (v->data.set.elem_type)
            east_type_release(v->data.set.elem_type);
        break;

    case EAST_VAL_DICT:
        for (size_t i = 0; i < v->data.dict.len; i++) {
            east_value_release(v->data.dict.keys[i]);
            east_value_release(v->data.dict.values[i]);
        }
        free(v->data.dict.keys);
        free(v->data.dict.values);
        if (v->data.dict.key_type)
            east_type_release(v->data.dict.key_type);
        if (v->data.dict.val_type)
            east_type_release(v->data.dict.val_type);
        break;

    case EAST_VAL_STRUCT:
        for (size_t i = 0; i < v->data.struct_.num_fields; i++) {
            free(v->data.struct_.field_names[i]);
            east_value_release(v->data.struct_.field_values[i]);
        }
        free(v->data.struct_.field_names);
        free(v->data.struct_.field_values);
        if (v->data.struct_.type)
            east_type_release(v->data.struct_.type);
        break;

    case EAST_VAL_VARIANT:
        free(v->data.variant.case_name);
        east_value_release(v->data.variant.value);
        if (v->data.variant.type)
            east_type_release(v->data.variant.type);
        break;

    case EAST_VAL_REF:
        east_value_release(v->data.ref.value);
        break;

    case EAST_VAL_VECTOR:
        free(v->data.vector.data);
        if (v->data.vector.elem_type)
            east_type_release(v->data.vector.elem_type);
        break;

    case EAST_VAL_MATRIX:
        free(v->data.matrix.data);
        if (v->data.matrix.elem_type)
            east_type_release(v->data.matrix.elem_type);
        break;

    case EAST_VAL_FUNCTION:
        if (v->data.function.compiled) {
            east_compiled_fn_free(v->data.function.compiled);
        }
        break;
    }

    free(v);
}

/* ------------------------------------------------------------------ */
/*  Structural equality                                                */
/* ------------------------------------------------------------------ */

bool east_value_equal(EastValue *a, EastValue *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    switch (a->kind) {
    case EAST_VAL_NULL:
        return true;

    case EAST_VAL_BOOLEAN:
        return a->data.boolean == b->data.boolean;

    case EAST_VAL_INTEGER:
        return a->data.integer == b->data.integer;

    case EAST_VAL_FLOAT:
        /* equalFor semantics: NaN equals NaN, +0 != -0 (Object.is) */
        if (isnan(a->data.float64)) return isnan(b->data.float64) ? true : false;
        if (isnan(b->data.float64)) return false;
        if (a->data.float64 == 0.0 && b->data.float64 == 0.0)
            return signbit(a->data.float64) == signbit(b->data.float64);
        return a->data.float64 == b->data.float64;

    case EAST_VAL_STRING:
        if (a->data.string.len != b->data.string.len) return false;
        return memcmp(a->data.string.data, b->data.string.data,
                      a->data.string.len) == 0;

    case EAST_VAL_DATETIME:
        return a->data.datetime == b->data.datetime;

    case EAST_VAL_BLOB:
        if (a->data.blob.len != b->data.blob.len) return false;
        if (a->data.blob.len == 0) return true;
        return memcmp(a->data.blob.data, b->data.blob.data,
                      a->data.blob.len) == 0;

    case EAST_VAL_ARRAY:
        if (a->data.array.len != b->data.array.len) return false;
        for (size_t i = 0; i < a->data.array.len; i++) {
            if (!east_value_equal(a->data.array.items[i],
                                  b->data.array.items[i]))
                return false;
        }
        return true;

    case EAST_VAL_SET:
        if (a->data.set.len != b->data.set.len) return false;
        for (size_t i = 0; i < a->data.set.len; i++) {
            if (!east_value_equal(a->data.set.items[i],
                                  b->data.set.items[i]))
                return false;
        }
        return true;

    case EAST_VAL_DICT:
        if (a->data.dict.len != b->data.dict.len) return false;
        for (size_t i = 0; i < a->data.dict.len; i++) {
            if (!east_value_equal(a->data.dict.keys[i],
                                  b->data.dict.keys[i]))
                return false;
            if (!east_value_equal(a->data.dict.values[i],
                                  b->data.dict.values[i]))
                return false;
        }
        return true;

    case EAST_VAL_STRUCT:
        if (a->data.struct_.num_fields != b->data.struct_.num_fields)
            return false;
        for (size_t i = 0; i < a->data.struct_.num_fields; i++) {
            if (strcmp(a->data.struct_.field_names[i],
                       b->data.struct_.field_names[i]) != 0)
                return false;
            if (!east_value_equal(a->data.struct_.field_values[i],
                                  b->data.struct_.field_values[i]))
                return false;
        }
        return true;

    case EAST_VAL_VARIANT:
        if (strcmp(a->data.variant.case_name,
                   b->data.variant.case_name) != 0)
            return false;
        return east_value_equal(a->data.variant.value, b->data.variant.value);

    case EAST_VAL_REF:
        return east_value_equal(a->data.ref.value, b->data.ref.value);

    case EAST_VAL_VECTOR: {
        if (a->data.vector.len != b->data.vector.len) return false;
        size_t n = a->data.vector.len;
        size_t esize = elem_size_for_type(a->data.vector.elem_type);
        if (n == 0) return true;
        return memcmp(a->data.vector.data, b->data.vector.data,
                      n * esize) == 0;
    }

    case EAST_VAL_MATRIX: {
        if (a->data.matrix.rows != b->data.matrix.rows) return false;
        if (a->data.matrix.cols != b->data.matrix.cols) return false;
        size_t n = a->data.matrix.rows * a->data.matrix.cols;
        size_t esize = elem_size_for_type(a->data.matrix.elem_type);
        if (n == 0) return true;
        return memcmp(a->data.matrix.data, b->data.matrix.data,
                      n * esize) == 0;
    }

    case EAST_VAL_FUNCTION:
        return a->data.function.compiled == b->data.function.compiled;
    }

    return false;
}

/* ------------------------------------------------------------------ */
/*  Total ordering                                                     */
/* ------------------------------------------------------------------ */

/*
 * Kind ordering:
 *   NULL < BOOLEAN < INTEGER < FLOAT < STRING < DATETIME < BLOB
 *   < ARRAY < SET < DICT < STRUCT < VARIANT
 * (remaining kinds are placed after VARIANT in enum order)
 */
static int kind_rank(EastValueKind k) {
    switch (k) {
    case EAST_VAL_NULL:     return 0;
    case EAST_VAL_BOOLEAN:  return 1;
    case EAST_VAL_INTEGER:  return 2;
    case EAST_VAL_FLOAT:    return 3;
    case EAST_VAL_STRING:   return 4;
    case EAST_VAL_DATETIME: return 5;
    case EAST_VAL_BLOB:     return 6;
    case EAST_VAL_ARRAY:    return 7;
    case EAST_VAL_SET:      return 8;
    case EAST_VAL_DICT:     return 9;
    case EAST_VAL_STRUCT:   return 10;
    case EAST_VAL_VARIANT:  return 11;
    case EAST_VAL_REF:      return 12;
    case EAST_VAL_VECTOR:   return 13;
    case EAST_VAL_MATRIX:   return 14;
    case EAST_VAL_FUNCTION: return 15;
    }
    return (int)k;
}

static int cmp_int64(int64_t a, int64_t b) {
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

static int cmp_double(double a, double b) {
    /* NaN is greatest */
    if (isnan(a)) return isnan(b) ? 0 : 1;
    if (isnan(b)) return -1;
    /* -0 < +0 */
    if (a == 0.0 && b == 0.0) {
        int a_neg = signbit(a) != 0;
        int b_neg = signbit(b) != 0;
        if (a_neg && !b_neg) return -1;
        if (!a_neg && b_neg) return 1;
        return 0;
    }
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

static int cmp_size(size_t a, size_t b) {
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

int east_value_compare(EastValue *a, EastValue *b) {
    if (a == b) return 0;

    /* Handle NULLs (C pointer null, not EAST_VAL_NULL). */
    if (!a) return -1;
    if (!b) return 1;

    int ra = kind_rank(a->kind);
    int rb = kind_rank(b->kind);
    if (ra != rb) return (ra < rb) ? -1 : 1;

    switch (a->kind) {
    case EAST_VAL_NULL:
        return 0;

    case EAST_VAL_BOOLEAN:
        /* false < true */
        if (a->data.boolean == b->data.boolean) return 0;
        return a->data.boolean ? 1 : -1;

    case EAST_VAL_INTEGER:
        return cmp_int64(a->data.integer, b->data.integer);

    case EAST_VAL_FLOAT:
        return cmp_double(a->data.float64, b->data.float64);

    case EAST_VAL_STRING: {
        size_t min_len = a->data.string.len < b->data.string.len
                             ? a->data.string.len
                             : b->data.string.len;
        int c = memcmp(a->data.string.data, b->data.string.data, min_len);
        if (c != 0) return (c < 0) ? -1 : 1;
        return cmp_size(a->data.string.len, b->data.string.len);
    }

    case EAST_VAL_DATETIME:
        return cmp_int64(a->data.datetime, b->data.datetime);

    case EAST_VAL_BLOB: {
        size_t min_len = a->data.blob.len < b->data.blob.len
                             ? a->data.blob.len
                             : b->data.blob.len;
        if (min_len > 0) {
            int c = memcmp(a->data.blob.data, b->data.blob.data, min_len);
            if (c != 0) return (c < 0) ? -1 : 1;
        }
        return cmp_size(a->data.blob.len, b->data.blob.len);
    }

    case EAST_VAL_ARRAY: {
        size_t min_len = a->data.array.len < b->data.array.len
                             ? a->data.array.len
                             : b->data.array.len;
        for (size_t i = 0; i < min_len; i++) {
            int c = east_value_compare(a->data.array.items[i],
                                       b->data.array.items[i]);
            if (c != 0) return c;
        }
        return cmp_size(a->data.array.len, b->data.array.len);
    }

    case EAST_VAL_SET: {
        size_t min_len = a->data.set.len < b->data.set.len
                             ? a->data.set.len
                             : b->data.set.len;
        for (size_t i = 0; i < min_len; i++) {
            int c = east_value_compare(a->data.set.items[i],
                                       b->data.set.items[i]);
            if (c != 0) return c;
        }
        return cmp_size(a->data.set.len, b->data.set.len);
    }

    case EAST_VAL_DICT: {
        size_t min_len = a->data.dict.len < b->data.dict.len
                             ? a->data.dict.len
                             : b->data.dict.len;
        for (size_t i = 0; i < min_len; i++) {
            int c = east_value_compare(a->data.dict.keys[i],
                                       b->data.dict.keys[i]);
            if (c != 0) return c;
            c = east_value_compare(a->data.dict.values[i],
                                   b->data.dict.values[i]);
            if (c != 0) return c;
        }
        return cmp_size(a->data.dict.len, b->data.dict.len);
    }

    case EAST_VAL_STRUCT: {
        int c = cmp_size(a->data.struct_.num_fields,
                         b->data.struct_.num_fields);
        if (c != 0) return c;
        for (size_t i = 0; i < a->data.struct_.num_fields; i++) {
            c = strcmp(a->data.struct_.field_names[i],
                       b->data.struct_.field_names[i]);
            if (c != 0) return (c < 0) ? -1 : 1;
            c = east_value_compare(a->data.struct_.field_values[i],
                                   b->data.struct_.field_values[i]);
            if (c != 0) return c;
        }
        return 0;
    }

    case EAST_VAL_VARIANT: {
        int c = strcmp(a->data.variant.case_name, b->data.variant.case_name);
        if (c != 0) return (c < 0) ? -1 : 1;
        return east_value_compare(a->data.variant.value,
                                  b->data.variant.value);
    }

    case EAST_VAL_REF:
        return east_value_compare(a->data.ref.value, b->data.ref.value);

    case EAST_VAL_VECTOR: {
        int c = cmp_size(a->data.vector.len, b->data.vector.len);
        if (c != 0) return c;
        size_t n = a->data.vector.len;
        size_t esize = elem_size_for_type(a->data.vector.elem_type);
        if (n == 0) return 0;
        int m = memcmp(a->data.vector.data, b->data.vector.data, n * esize);
        if (m < 0) return -1;
        if (m > 0) return 1;
        return 0;
    }

    case EAST_VAL_MATRIX: {
        int c = cmp_size(a->data.matrix.rows, b->data.matrix.rows);
        if (c != 0) return c;
        c = cmp_size(a->data.matrix.cols, b->data.matrix.cols);
        if (c != 0) return c;
        size_t n = a->data.matrix.rows * a->data.matrix.cols;
        size_t esize = elem_size_for_type(a->data.matrix.elem_type);
        if (n == 0) return 0;
        int m = memcmp(a->data.matrix.data, b->data.matrix.data, n * esize);
        if (m < 0) return -1;
        if (m > 0) return 1;
        return 0;
    }

    case EAST_VAL_FUNCTION:
        if (a->data.function.compiled < b->data.function.compiled) return -1;
        if (a->data.function.compiled > b->data.function.compiled) return 1;
        return 0;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Printing                                                           */
/* ------------------------------------------------------------------ */

/*
 * Helper: append to buffer safely. Returns number of characters that
 * *would* have been written (like snprintf). `pos` is the current write
 * position, `buf_size` is total capacity.
 */
static int buf_append(char *buf, size_t buf_size, int pos, const char *fmt,
                      ...) __attribute__((format(printf, 4, 5)));
static int buf_append(char *buf, size_t buf_size, int pos, const char *fmt,
                      ...) {
    va_list ap;
    va_start(ap, fmt);
    int remaining = (int)buf_size - pos;
    int n;
    if (remaining > 0) {
        n = vsnprintf(buf + pos, (size_t)remaining, fmt, ap);
    } else {
        /* Buffer already full; measure how much space we would need. */
        n = vsnprintf(NULL, 0, fmt, ap);
    }
    va_end(ap);
    return (n > 0) ? n : 0;
}

static int print_value(EastValue *v, char *buf, size_t buf_size, int pos) {
    if (!v) {
        return pos + buf_append(buf, buf_size, pos, "null");
    }

    switch (v->kind) {
    case EAST_VAL_NULL:
        return pos + buf_append(buf, buf_size, pos, "null");

    case EAST_VAL_BOOLEAN:
        return pos + buf_append(buf, buf_size, pos, "%s",
                                v->data.boolean ? "true" : "false");

    case EAST_VAL_INTEGER:
        return pos + buf_append(buf, buf_size, pos, "%" PRId64,
                                v->data.integer);

    case EAST_VAL_FLOAT: {
        char numbuf[64];
        east_fmt_double(numbuf, sizeof(numbuf), v->data.float64);
        return pos + buf_append(buf, buf_size, pos, "%s", numbuf);
    }

    case EAST_VAL_STRING: {
        pos += buf_append(buf, buf_size, pos, "\"");
        for (size_t i = 0; i < v->data.string.len; i++) {
            char c = v->data.string.data[i];
            switch (c) {
            case '"':  pos += buf_append(buf, buf_size, pos, "\\\""); break;
            case '\\': pos += buf_append(buf, buf_size, pos, "\\\\"); break;
            case '\n': pos += buf_append(buf, buf_size, pos, "\\n");  break;
            case '\r': pos += buf_append(buf, buf_size, pos, "\\r");  break;
            case '\t': pos += buf_append(buf, buf_size, pos, "\\t");  break;
            default:
                if ((unsigned char)c < 0x20) {
                    pos += buf_append(buf, buf_size, pos, "\\u%04x",
                                      (unsigned char)c);
                } else {
                    pos += buf_append(buf, buf_size, pos, "%c", c);
                }
                break;
            }
        }
        pos += buf_append(buf, buf_size, pos, "\"");
        return pos;
    }

    case EAST_VAL_DATETIME:
        return pos + buf_append(buf, buf_size, pos, "%" PRId64,
                                v->data.datetime);

    case EAST_VAL_BLOB: {
        pos += buf_append(buf, buf_size, pos, "0x");
        for (size_t i = 0; i < v->data.blob.len; i++) {
            pos += buf_append(buf, buf_size, pos, "%02x",
                              v->data.blob.data[i]);
        }
        return pos;
    }

    case EAST_VAL_ARRAY: {
        pos += buf_append(buf, buf_size, pos, "[");
        for (size_t i = 0; i < v->data.array.len; i++) {
            if (i > 0) pos += buf_append(buf, buf_size, pos, ", ");
            pos = print_value(v->data.array.items[i], buf, buf_size, pos);
        }
        pos += buf_append(buf, buf_size, pos, "]");
        return pos;
    }

    case EAST_VAL_SET: {
        pos += buf_append(buf, buf_size, pos, "{");
        for (size_t i = 0; i < v->data.set.len; i++) {
            if (i > 0) pos += buf_append(buf, buf_size, pos, ", ");
            pos = print_value(v->data.set.items[i], buf, buf_size, pos);
        }
        pos += buf_append(buf, buf_size, pos, "}");
        return pos;
    }

    case EAST_VAL_DICT: {
        pos += buf_append(buf, buf_size, pos, "{");
        for (size_t i = 0; i < v->data.dict.len; i++) {
            if (i > 0) pos += buf_append(buf, buf_size, pos, ", ");
            pos = print_value(v->data.dict.keys[i], buf, buf_size, pos);
            pos += buf_append(buf, buf_size, pos, ": ");
            pos = print_value(v->data.dict.values[i], buf, buf_size, pos);
        }
        pos += buf_append(buf, buf_size, pos, "}");
        return pos;
    }

    case EAST_VAL_STRUCT: {
        pos += buf_append(buf, buf_size, pos, "{");
        for (size_t i = 0; i < v->data.struct_.num_fields; i++) {
            if (i > 0) pos += buf_append(buf, buf_size, pos, ", ");
            pos += buf_append(buf, buf_size, pos, "%s: ",
                              v->data.struct_.field_names[i]);
            pos = print_value(v->data.struct_.field_values[i], buf, buf_size,
                              pos);
        }
        pos += buf_append(buf, buf_size, pos, "}");
        return pos;
    }

    case EAST_VAL_VARIANT: {
        pos += buf_append(buf, buf_size, pos, ".%s",
                          v->data.variant.case_name);
        if (v->data.variant.value &&
            v->data.variant.value->kind != EAST_VAL_NULL) {
            pos += buf_append(buf, buf_size, pos, " ");
            pos = print_value(v->data.variant.value, buf, buf_size, pos);
        }
        return pos;
    }

    case EAST_VAL_REF: {
        pos += buf_append(buf, buf_size, pos, "ref(");
        pos = print_value(v->data.ref.value, buf, buf_size, pos);
        pos += buf_append(buf, buf_size, pos, ")");
        return pos;
    }

    case EAST_VAL_VECTOR: {
        pos += buf_append(buf, buf_size, pos, "[");
        size_t n = v->data.vector.len;
        EastType *et = v->data.vector.elem_type;
        for (size_t i = 0; i < n; i++) {
            if (i > 0) pos += buf_append(buf, buf_size, pos, ", ");
            if (et && et->kind == EAST_TYPE_INTEGER) {
                int64_t *arr = (int64_t *)v->data.vector.data;
                pos += buf_append(buf, buf_size, pos, "%" PRId64, arr[i]);
            } else if (et && et->kind == EAST_TYPE_BOOLEAN) {
                bool *arr = (bool *)v->data.vector.data;
                pos += buf_append(buf, buf_size, pos, "%s",
                                  arr[i] ? "true" : "false");
            } else {
                double *arr = (double *)v->data.vector.data;
                { char nb[64]; east_fmt_double(nb, sizeof(nb), arr[i]);
                pos += buf_append(buf, buf_size, pos, "%s", nb); }
            }
        }
        pos += buf_append(buf, buf_size, pos, "]");
        return pos;
    }

    case EAST_VAL_MATRIX: {
        size_t rows = v->data.matrix.rows;
        size_t cols = v->data.matrix.cols;
        EastType *et = v->data.matrix.elem_type;
        pos += buf_append(buf, buf_size, pos, "[");
        for (size_t r = 0; r < rows; r++) {
            if (r > 0) pos += buf_append(buf, buf_size, pos, ", ");
            pos += buf_append(buf, buf_size, pos, "[");
            for (size_t c = 0; c < cols; c++) {
                if (c > 0) pos += buf_append(buf, buf_size, pos, ", ");
                size_t idx = r * cols + c;
                if (et && et->kind == EAST_TYPE_INTEGER) {
                    int64_t *arr = (int64_t *)v->data.matrix.data;
                    pos += buf_append(buf, buf_size, pos, "%" PRId64,
                                      arr[idx]);
                } else if (et && et->kind == EAST_TYPE_BOOLEAN) {
                    bool *arr = (bool *)v->data.matrix.data;
                    pos += buf_append(buf, buf_size, pos, "%s",
                                      arr[idx] ? "true" : "false");
                } else {
                    double *arr = (double *)v->data.matrix.data;
                    { char nb[64]; east_fmt_double(nb, sizeof(nb), arr[idx]);
                    pos += buf_append(buf, buf_size, pos, "%s", nb); }
                }
            }
            pos += buf_append(buf, buf_size, pos, "]");
        }
        pos += buf_append(buf, buf_size, pos, "]");
        return pos;
    }

    case EAST_VAL_FUNCTION:
        return pos + buf_append(buf, buf_size, pos, "<function>");
    }

    return pos;
}

int east_value_print(EastValue *v, char *buf, size_t buf_size) {
    int written = print_value(v, buf, buf_size, 0);
    /* Ensure null termination. */
    if (buf && buf_size > 0) {
        if ((size_t)written >= buf_size) {
            buf[buf_size - 1] = '\0';
        }
    }
    return written;
}

/* ------------------------------------------------------------------ */
/*  Kind name helper                                                   */
/* ------------------------------------------------------------------ */

const char *east_value_kind_name(EastValueKind kind) {
    switch (kind) {
    case EAST_VAL_NULL:     return "Null";
    case EAST_VAL_BOOLEAN:  return "Boolean";
    case EAST_VAL_INTEGER:  return "Integer";
    case EAST_VAL_FLOAT:    return "Float";
    case EAST_VAL_STRING:   return "String";
    case EAST_VAL_DATETIME: return "DateTime";
    case EAST_VAL_BLOB:     return "Blob";
    case EAST_VAL_ARRAY:    return "Array";
    case EAST_VAL_SET:      return "Set";
    case EAST_VAL_DICT:     return "Dict";
    case EAST_VAL_STRUCT:   return "Struct";
    case EAST_VAL_VARIANT:  return "Variant";
    case EAST_VAL_REF:      return "Ref";
    case EAST_VAL_VECTOR:   return "Vector";
    case EAST_VAL_MATRIX:   return "Matrix";
    case EAST_VAL_FUNCTION: return "Function";
    }
    return "Unknown";
}
