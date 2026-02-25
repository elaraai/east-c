#include "east/types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Primitive type singletons (ref_count = -1: never freed)           */
/* ------------------------------------------------------------------ */

EastType east_never_type    = { .kind = EAST_TYPE_NEVER,    .ref_count = -1, .data = {0} };
EastType east_null_type     = { .kind = EAST_TYPE_NULL,     .ref_count = -1, .data = {0} };
EastType east_boolean_type  = { .kind = EAST_TYPE_BOOLEAN,  .ref_count = -1, .data = {0} };
EastType east_integer_type  = { .kind = EAST_TYPE_INTEGER,  .ref_count = -1, .data = {0} };
EastType east_float_type    = { .kind = EAST_TYPE_FLOAT,    .ref_count = -1, .data = {0} };
EastType east_string_type   = { .kind = EAST_TYPE_STRING,   .ref_count = -1, .data = {0} };
EastType east_datetime_type = { .kind = EAST_TYPE_DATETIME, .ref_count = -1, .data = {0} };
EastType east_blob_type     = { .kind = EAST_TYPE_BLOB,     .ref_count = -1, .data = {0} };

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static EastType *alloc_type(EastTypeKind kind)
{
    EastType *t = calloc(1, sizeof(EastType));
    if (!t) return NULL;
    t->kind = kind;
    t->ref_count = 1;
    return t;
}

static int field_cmp(const void *a, const void *b)
{
    const EastTypeField *fa = a;
    const EastTypeField *fb = b;
    return strcmp(fa->name, fb->name);
}

/* ------------------------------------------------------------------ */
/*  Constructors                                                       */
/* ------------------------------------------------------------------ */

EastType *east_array_type(EastType *elem)
{
    EastType *t = alloc_type(EAST_TYPE_ARRAY);
    if (!t) return NULL;
    east_type_retain(elem);
    t->data.element = elem;
    return t;
}

EastType *east_set_type(EastType *elem)
{
    EastType *t = alloc_type(EAST_TYPE_SET);
    if (!t) return NULL;
    east_type_retain(elem);
    t->data.element = elem;
    return t;
}

EastType *east_dict_type(EastType *key, EastType *val)
{
    EastType *t = alloc_type(EAST_TYPE_DICT);
    if (!t) return NULL;
    east_type_retain(key);
    east_type_retain(val);
    t->data.dict.key = key;
    t->data.dict.value = val;
    return t;
}

EastType *east_struct_type(const char **names, EastType **types, size_t count)
{
    EastType *t = alloc_type(EAST_TYPE_STRUCT);
    if (!t) return NULL;

    EastTypeField *fields = NULL;
    if (count > 0) {
        fields = calloc(count, sizeof(EastTypeField));
        if (!fields) { free(t); return NULL; }
        for (size_t i = 0; i < count; i++) {
            fields[i].name = strdup(names[i]);
            if (!fields[i].name) {
                for (size_t j = 0; j < i; j++) {
                    free(fields[j].name);
                    east_type_release(fields[j].type);
                }
                free(fields);
                free(t);
                return NULL;
            }
            east_type_retain(types[i]);
            fields[i].type = types[i];
        }
    }

    t->data.struct_.fields = fields;
    t->data.struct_.num_fields = count;
    return t;
}

EastType *east_variant_type(const char **names, EastType **types, size_t count)
{
    EastType *t = alloc_type(EAST_TYPE_VARIANT);
    if (!t) return NULL;

    EastTypeField *cases = NULL;
    if (count > 0) {
        cases = calloc(count, sizeof(EastTypeField));
        if (!cases) { free(t); return NULL; }
        for (size_t i = 0; i < count; i++) {
            cases[i].name = strdup(names[i]);
            if (!cases[i].name) {
                for (size_t j = 0; j < i; j++) {
                    free(cases[j].name);
                    east_type_release(cases[j].type);
                }
                free(cases);
                free(t);
                return NULL;
            }
            east_type_retain(types[i]);
            cases[i].type = types[i];
        }
        qsort(cases, count, sizeof(EastTypeField), field_cmp);
    }

    t->data.variant.cases = cases;
    t->data.variant.num_cases = count;
    return t;
}

EastType *east_ref_type(EastType *inner)
{
    EastType *t = alloc_type(EAST_TYPE_REF);
    if (!t) return NULL;
    east_type_retain(inner);
    t->data.element = inner;
    return t;
}

EastType *east_vector_type(EastType *elem)
{
    EastType *t = alloc_type(EAST_TYPE_VECTOR);
    if (!t) return NULL;
    east_type_retain(elem);
    t->data.element = elem;
    return t;
}

EastType *east_matrix_type(EastType *elem)
{
    EastType *t = alloc_type(EAST_TYPE_MATRIX);
    if (!t) return NULL;
    east_type_retain(elem);
    t->data.element = elem;
    return t;
}

EastType *east_function_type(EastType **inputs, size_t num_inputs, EastType *output)
{
    EastType *t = alloc_type(EAST_TYPE_FUNCTION);
    if (!t) return NULL;

    EastType **inp = NULL;
    if (num_inputs > 0) {
        inp = calloc(num_inputs, sizeof(EastType *));
        if (!inp) { free(t); return NULL; }
        for (size_t i = 0; i < num_inputs; i++) {
            east_type_retain(inputs[i]);
            inp[i] = inputs[i];
        }
    }

    east_type_retain(output);
    t->data.function.inputs = inp;
    t->data.function.num_inputs = num_inputs;
    t->data.function.output = output;
    return t;
}

EastType *east_async_function_type(EastType **inputs, size_t num_inputs, EastType *output)
{
    EastType *t = alloc_type(EAST_TYPE_ASYNC_FUNCTION);
    if (!t) return NULL;

    EastType **inp = NULL;
    if (num_inputs > 0) {
        inp = calloc(num_inputs, sizeof(EastType *));
        if (!inp) { free(t); return NULL; }
        for (size_t i = 0; i < num_inputs; i++) {
            east_type_retain(inputs[i]);
            inp[i] = inputs[i];
        }
    }

    east_type_retain(output);
    t->data.function.inputs = inp;
    t->data.function.num_inputs = num_inputs;
    t->data.function.output = output;
    return t;
}

EastType *east_recursive_type_new(void)
{
    EastType *t = alloc_type(EAST_TYPE_RECURSIVE);
    if (!t) return NULL;
    t->data.recursive.node = NULL;
    t->data.recursive.internal_refs = 0;
    return t;
}

void east_recursive_type_set(EastType *rec, EastType *node)
{
    if (!rec || rec->kind != EAST_TYPE_RECURSIVE) return;
    /* NOTE: we do NOT retain node here because the inner type tree
     * contains self-references back to rec, forming a cycle.
     * Call east_recursive_type_finalize() after this to enable
     * automatic cycle-aware cleanup. */
    rec->data.recursive.node = node;
}

/* Count back-references to `target` within the type tree rooted at `t`. */
static int count_back_refs(EastType *t, EastType *target)
{
    if (!t) return 0;
    if (t == target) return 1;

    switch (t->kind) {
    case EAST_TYPE_ARRAY:
    case EAST_TYPE_SET:
    case EAST_TYPE_REF:
    case EAST_TYPE_VECTOR:
    case EAST_TYPE_MATRIX:
        return count_back_refs(t->data.element, target);

    case EAST_TYPE_DICT:
        return count_back_refs(t->data.dict.key, target) +
               count_back_refs(t->data.dict.value, target);

    case EAST_TYPE_STRUCT:
        { int n = 0;
          for (size_t i = 0; i < t->data.struct_.num_fields; i++)
              n += count_back_refs(t->data.struct_.fields[i].type, target);
          return n; }

    case EAST_TYPE_VARIANT:
        { int n = 0;
          for (size_t i = 0; i < t->data.variant.num_cases; i++)
              n += count_back_refs(t->data.variant.cases[i].type, target);
          return n; }

    case EAST_TYPE_FUNCTION:
    case EAST_TYPE_ASYNC_FUNCTION:
        { int n = 0;
          for (size_t i = 0; i < t->data.function.num_inputs; i++)
              n += count_back_refs(t->data.function.inputs[i], target);
          n += count_back_refs(t->data.function.output, target);
          return n; }

    case EAST_TYPE_RECURSIVE:
        return count_back_refs(t->data.recursive.node, target);

    default:
        return 0;
    }
}

/* Replace all pointers to `target` with NULL within the type tree rooted at `t`.
 * This prevents dangling back-references when the target (a recursive wrapper)
 * is about to be freed.  Inner tree types that are still alive (retained by
 * values) will safely release NULL children instead of the freed wrapper. */
static void nullify_back_refs(EastType *t, EastType *target)
{
    if (!t || t == target) return;

    switch (t->kind) {
    case EAST_TYPE_ARRAY:
    case EAST_TYPE_SET:
    case EAST_TYPE_REF:
    case EAST_TYPE_VECTOR:
    case EAST_TYPE_MATRIX:
        if (t->data.element == target)
            t->data.element = NULL;
        else
            nullify_back_refs(t->data.element, target);
        break;

    case EAST_TYPE_DICT:
        if (t->data.dict.key == target) t->data.dict.key = NULL;
        else nullify_back_refs(t->data.dict.key, target);
        if (t->data.dict.value == target) t->data.dict.value = NULL;
        else nullify_back_refs(t->data.dict.value, target);
        break;

    case EAST_TYPE_STRUCT:
        for (size_t i = 0; i < t->data.struct_.num_fields; i++) {
            if (t->data.struct_.fields[i].type == target)
                t->data.struct_.fields[i].type = NULL;
            else
                nullify_back_refs(t->data.struct_.fields[i].type, target);
        }
        break;

    case EAST_TYPE_VARIANT:
        for (size_t i = 0; i < t->data.variant.num_cases; i++) {
            if (t->data.variant.cases[i].type == target)
                t->data.variant.cases[i].type = NULL;
            else
                nullify_back_refs(t->data.variant.cases[i].type, target);
        }
        break;

    case EAST_TYPE_FUNCTION:
    case EAST_TYPE_ASYNC_FUNCTION:
        for (size_t i = 0; i < t->data.function.num_inputs; i++) {
            if (t->data.function.inputs[i] == target)
                t->data.function.inputs[i] = NULL;
            else
                nullify_back_refs(t->data.function.inputs[i], target);
        }
        if (t->data.function.output == target)
            t->data.function.output = NULL;
        else
            nullify_back_refs(t->data.function.output, target);
        break;

    case EAST_TYPE_RECURSIVE:
        if (t->data.recursive.node == target)
            t->data.recursive.node = NULL;
        else
            nullify_back_refs(t->data.recursive.node, target);
        break;

    default:
        break;
    }
}

void east_recursive_type_finalize(EastType *rec)
{
    if (!rec || rec->kind != EAST_TYPE_RECURSIVE) return;
    if (rec->ref_count <= 0) return;  /* singleton or invalid */

    /* Walk the inner tree to count actual back-references to this wrapper.
     * This is more robust than assuming ref_count - 1, because the wrapper
     * may have been retained externally before finalize is called. */
    int internal = count_back_refs(rec->data.recursive.node, rec);
    rec->data.recursive.internal_refs = internal;
    rec->ref_count -= internal;
}

/* ------------------------------------------------------------------ */
/*  Ref counting                                                       */
/* ------------------------------------------------------------------ */

void east_type_retain(EastType *t)
{
    if (!t) return;
    if (t->ref_count < 0) return;   /* singleton -- never freed */
    t->ref_count++;
}

void east_type_release(EastType *t)
{
    if (!t) return;
    if (t->ref_count == -1) return;   /* singleton -- never freed */

    /* Sentinel: recursive type wrapper being destroyed.
     * This release is a back-reference from the inner tree being torn down.
     * Don't free here — the RECURSIVE case in east_type_release (still on
     * the call stack) will free the wrapper after the inner tree is done. */
    if (t->ref_count == -2) {
        return;
    }

    t->ref_count--;
    if (t->ref_count > 0) return;

    /* ref_count reached 0 -- free children then the node itself */
    switch (t->kind) {
    case EAST_TYPE_ARRAY:
    case EAST_TYPE_SET:
    case EAST_TYPE_REF:
    case EAST_TYPE_VECTOR:
    case EAST_TYPE_MATRIX:
        east_type_release(t->data.element);
        break;

    case EAST_TYPE_DICT:
        east_type_release(t->data.dict.key);
        east_type_release(t->data.dict.value);
        break;

    case EAST_TYPE_STRUCT:
        for (size_t i = 0; i < t->data.struct_.num_fields; i++) {
            free(t->data.struct_.fields[i].name);
            east_type_release(t->data.struct_.fields[i].type);
        }
        free(t->data.struct_.fields);
        break;

    case EAST_TYPE_VARIANT:
        for (size_t i = 0; i < t->data.variant.num_cases; i++) {
            free(t->data.variant.cases[i].name);
            east_type_release(t->data.variant.cases[i].type);
        }
        free(t->data.variant.cases);
        break;

    case EAST_TYPE_FUNCTION:
    case EAST_TYPE_ASYNC_FUNCTION:
        for (size_t i = 0; i < t->data.function.num_inputs; i++) {
            east_type_release(t->data.function.inputs[i]);
        }
        free(t->data.function.inputs);
        east_type_release(t->data.function.output);
        break;

    case EAST_TYPE_RECURSIVE: {
        /* Cycle-breaking: nullify back-refs, release inner tree, then free.
         * nullify_back_refs replaces all pointers to this wrapper within the
         * inner tree with NULL.  This prevents use-after-free when inner tree
         * types outlive the wrapper (e.g. retained by decoded values). */
        EastType *inner = t->data.recursive.node;
        t->data.recursive.node = NULL;
        t->ref_count = -2;  /* sentinel: safety net for any missed back-refs */
        if (inner) {
            nullify_back_refs(inner, t);
            east_type_release(inner);
        }
        free(t);
        return;  /* skip the free(t) below */
    }

    default:
        /* primitives have no children to release */
        break;
    }

    free(t);
}

/* ------------------------------------------------------------------ */
/*  Structural equality                                                */
/* ------------------------------------------------------------------ */

bool east_type_equal(EastType *a, EastType *b)
{
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    switch (a->kind) {
    /* Primitives: kind match is sufficient */
    case EAST_TYPE_NEVER:
    case EAST_TYPE_NULL:
    case EAST_TYPE_BOOLEAN:
    case EAST_TYPE_INTEGER:
    case EAST_TYPE_FLOAT:
    case EAST_TYPE_STRING:
    case EAST_TYPE_DATETIME:
    case EAST_TYPE_BLOB:
        return true;

    /* Single element types */
    case EAST_TYPE_ARRAY:
    case EAST_TYPE_SET:
    case EAST_TYPE_REF:
    case EAST_TYPE_VECTOR:
    case EAST_TYPE_MATRIX:
        return east_type_equal(a->data.element, b->data.element);

    case EAST_TYPE_DICT:
        return east_type_equal(a->data.dict.key, b->data.dict.key) &&
               east_type_equal(a->data.dict.value, b->data.dict.value);

    case EAST_TYPE_STRUCT:
        if (a->data.struct_.num_fields != b->data.struct_.num_fields)
            return false;
        for (size_t i = 0; i < a->data.struct_.num_fields; i++) {
            if (strcmp(a->data.struct_.fields[i].name,
                       b->data.struct_.fields[i].name) != 0)
                return false;
            if (!east_type_equal(a->data.struct_.fields[i].type,
                                 b->data.struct_.fields[i].type))
                return false;
        }
        return true;

    case EAST_TYPE_VARIANT:
        if (a->data.variant.num_cases != b->data.variant.num_cases)
            return false;
        for (size_t i = 0; i < a->data.variant.num_cases; i++) {
            if (strcmp(a->data.variant.cases[i].name,
                       b->data.variant.cases[i].name) != 0)
                return false;
            if (!east_type_equal(a->data.variant.cases[i].type,
                                 b->data.variant.cases[i].type))
                return false;
        }
        return true;

    case EAST_TYPE_FUNCTION:
    case EAST_TYPE_ASYNC_FUNCTION:
        if (a->data.function.num_inputs != b->data.function.num_inputs)
            return false;
        for (size_t i = 0; i < a->data.function.num_inputs; i++) {
            if (!east_type_equal(a->data.function.inputs[i],
                                 b->data.function.inputs[i]))
                return false;
        }
        return east_type_equal(a->data.function.output,
                               b->data.function.output);

    case EAST_TYPE_RECURSIVE:
        /* Recursive types are equal if they're the same object (pointer equality)
         * or if their inner nodes are structurally equal.
         * Since self-references create cycles, rely on pointer equality. */
        return false; /* a != b already checked above */
    }

    return false;
}

/* ------------------------------------------------------------------ */
/*  Printing                                                           */
/* ------------------------------------------------------------------ */

int east_type_print(EastType *t, char *buf, size_t buf_size)
{
    if (!t) return snprintf(buf, buf_size, "(null)");

    switch (t->kind) {
    case EAST_TYPE_NEVER:
        return snprintf(buf, buf_size, "Never");
    case EAST_TYPE_NULL:
        return snprintf(buf, buf_size, "Null");
    case EAST_TYPE_BOOLEAN:
        return snprintf(buf, buf_size, "Boolean");
    case EAST_TYPE_INTEGER:
        return snprintf(buf, buf_size, "Integer");
    case EAST_TYPE_FLOAT:
        return snprintf(buf, buf_size, "Float");
    case EAST_TYPE_STRING:
        return snprintf(buf, buf_size, "String");
    case EAST_TYPE_DATETIME:
        return snprintf(buf, buf_size, "DateTime");
    case EAST_TYPE_BLOB:
        return snprintf(buf, buf_size, "Blob");

    case EAST_TYPE_ARRAY: {
        char inner[256];
        east_type_print(t->data.element, inner, sizeof(inner));
        return snprintf(buf, buf_size, "Array<%s>", inner);
    }

    case EAST_TYPE_SET: {
        char inner[256];
        east_type_print(t->data.element, inner, sizeof(inner));
        return snprintf(buf, buf_size, "Set<%s>", inner);
    }

    case EAST_TYPE_VECTOR: {
        char inner[256];
        east_type_print(t->data.element, inner, sizeof(inner));
        return snprintf(buf, buf_size, "Vector<%s>", inner);
    }

    case EAST_TYPE_MATRIX: {
        char inner[256];
        east_type_print(t->data.element, inner, sizeof(inner));
        return snprintf(buf, buf_size, "Matrix<%s>", inner);
    }

    case EAST_TYPE_REF: {
        char inner[256];
        east_type_print(t->data.element, inner, sizeof(inner));
        return snprintf(buf, buf_size, "Ref<%s>", inner);
    }

    case EAST_TYPE_DICT: {
        char kbuf[256], vbuf[256];
        east_type_print(t->data.dict.key, kbuf, sizeof(kbuf));
        east_type_print(t->data.dict.value, vbuf, sizeof(vbuf));
        return snprintf(buf, buf_size, "Dict<%s, %s>", kbuf, vbuf);
    }

    case EAST_TYPE_STRUCT: {
        int written = snprintf(buf, buf_size, "Struct { ");
        for (size_t i = 0; i < t->data.struct_.num_fields; i++) {
            char fbuf[256];
            east_type_print(t->data.struct_.fields[i].type, fbuf, sizeof(fbuf));
            if (i > 0) {
                written += snprintf(buf + written,
                                    buf_size > (size_t)written ? buf_size - written : 0,
                                    ", ");
            }
            written += snprintf(buf + written,
                                buf_size > (size_t)written ? buf_size - written : 0,
                                "%s: %s", t->data.struct_.fields[i].name, fbuf);
        }
        written += snprintf(buf + written,
                            buf_size > (size_t)written ? buf_size - written : 0,
                            " }");
        return written;
    }

    case EAST_TYPE_VARIANT: {
        int written = snprintf(buf, buf_size, "Variant { ");
        for (size_t i = 0; i < t->data.variant.num_cases; i++) {
            char cbuf[256];
            east_type_print(t->data.variant.cases[i].type, cbuf, sizeof(cbuf));
            if (i > 0) {
                written += snprintf(buf + written,
                                    buf_size > (size_t)written ? buf_size - written : 0,
                                    " | ");
            }
            written += snprintf(buf + written,
                                buf_size > (size_t)written ? buf_size - written : 0,
                                "%s: %s", t->data.variant.cases[i].name, cbuf);
        }
        written += snprintf(buf + written,
                            buf_size > (size_t)written ? buf_size - written : 0,
                            " }");
        return written;
    }

    case EAST_TYPE_FUNCTION:
    case EAST_TYPE_ASYNC_FUNCTION: {
        const char *prefix = (t->kind == EAST_TYPE_ASYNC_FUNCTION)
                             ? "AsyncFunction" : "Function";
        int written = snprintf(buf, buf_size, "%s(", prefix);
        for (size_t i = 0; i < t->data.function.num_inputs; i++) {
            char ibuf[256];
            east_type_print(t->data.function.inputs[i], ibuf, sizeof(ibuf));
            if (i > 0) {
                written += snprintf(buf + written,
                                    buf_size > (size_t)written ? buf_size - written : 0,
                                    ", ");
            }
            written += snprintf(buf + written,
                                buf_size > (size_t)written ? buf_size - written : 0,
                                "%s", ibuf);
        }
        char obuf[256];
        east_type_print(t->data.function.output, obuf, sizeof(obuf));
        written += snprintf(buf + written,
                            buf_size > (size_t)written ? buf_size - written : 0,
                            ") -> %s", obuf);
        return written;
    }

    case EAST_TYPE_RECURSIVE:
        if (t->data.recursive.node) {
            return snprintf(buf, buf_size, "Recursive(...)");
        }
        return snprintf(buf, buf_size, "Recursive(empty)");
    }

    return snprintf(buf, buf_size, "Unknown");
}

/* ------------------------------------------------------------------ */
/*  Kind name helper                                                   */
/* ------------------------------------------------------------------ */

const char *east_type_kind_name(EastTypeKind kind)
{
    switch (kind) {
    case EAST_TYPE_NEVER:          return "Never";
    case EAST_TYPE_NULL:           return "Null";
    case EAST_TYPE_BOOLEAN:        return "Boolean";
    case EAST_TYPE_INTEGER:        return "Integer";
    case EAST_TYPE_FLOAT:          return "Float";
    case EAST_TYPE_STRING:         return "String";
    case EAST_TYPE_DATETIME:       return "DateTime";
    case EAST_TYPE_BLOB:           return "Blob";
    case EAST_TYPE_ARRAY:          return "Array";
    case EAST_TYPE_SET:            return "Set";
    case EAST_TYPE_DICT:           return "Dict";
    case EAST_TYPE_STRUCT:         return "Struct";
    case EAST_TYPE_VARIANT:        return "Variant";
    case EAST_TYPE_REF:            return "Ref";
    case EAST_TYPE_VECTOR:         return "Vector";
    case EAST_TYPE_MATRIX:         return "Matrix";
    case EAST_TYPE_FUNCTION:       return "Function";
    case EAST_TYPE_ASYNC_FUNCTION: return "AsyncFunction";
    case EAST_TYPE_RECURSIVE:      return "Recursive";
    }
    return "Unknown";
}
