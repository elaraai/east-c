/*
 * Patch builtin functions: Diff, ApplyPatch, ComposePatch, InvertPatch.
 *
 * Implements the full East patch system with type-aware structural diffing.
 * Each patch is a Variant with cases:
 *   unchanged: Null
 *   replace: Struct{after: T, before: T}
 *   patch: <type-specific structural patch>  (containers only)
 */
#include "east/builtins.h"
#include "east/types.h"
#include "east/values.h"

#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/*  Type context: set by factory, read by impl                         */
/* ================================================================== */

static _Thread_local EastType *s_patch_type = NULL;

/* ================================================================== */
/*  Recursive type tracking                                            */
/* ================================================================== */

#define MAX_REC_DEPTH 32
static _Thread_local EastType *rec_stack[MAX_REC_DEPTH];
static _Thread_local int rec_depth = 0;

static bool in_rec_stack(EastType *t) {
    for (int i = 0; i < rec_depth; i++)
        if (rec_stack[i] == t) return true;
    return false;
}

/* Resolve type, unwrapping one level of recursion if not yet seen.
 * Returns the effective type to dispatch on.
 * Sets *replace_only = true if this is a recursive self-reference. */
static EastType *resolve_type(EastType *t, bool *replace_only) {
    *replace_only = false;
    if (!t) { *replace_only = true; return t; }
    if (t->kind != EAST_TYPE_RECURSIVE) return t;
    if (in_rec_stack(t)) {
        *replace_only = true;
        return t;
    }
    /* First encounter of this recursive wrapper — unwrap */
    if (rec_depth < MAX_REC_DEPTH)
        rec_stack[rec_depth++] = t;
    return t->data.recursive.node ? t->data.recursive.node : t;
}

static void pop_rec_if_pushed(EastType *t) {
    if (t && t->kind == EAST_TYPE_RECURSIVE &&
        rec_depth > 0 && rec_stack[rec_depth - 1] == t)
        rec_depth--;
}

/* ================================================================== */
/*  Variant helpers                                                    */
/* ================================================================== */

static EastValue *mk_unchanged(void) {
    return east_variant_new("unchanged", east_null(), NULL);
}

static EastValue *mk_replace(EastValue *before, EastValue *after) {
    const char *fn[] = {"after", "before"};
    EastValue *fv[] = {after, before};
    EastValue *s = east_struct_new(fn, fv, 2, NULL);
    EastValue *v = east_variant_new("replace", s, NULL);
    east_value_release(s);
    return v;
}

static EastValue *mk_patch_v(EastValue *inner) {
    EastValue *v = east_variant_new("patch", inner, NULL);
    east_value_release(inner);
    return v;
}

static bool is_tag(EastValue *v, const char *tag) {
    return v && v->kind == EAST_VAL_VARIANT &&
           strcmp(v->data.variant.case_name, tag) == 0;
}

static EastValue *patch_payload(EastValue *v) {
    return v->data.variant.value;
}

static EastValue *replace_before(EastValue *v) {
    return east_struct_get_field(patch_payload(v), "before");
}

static EastValue *replace_after(EastValue *v) {
    return east_struct_get_field(patch_payload(v), "after");
}

/* ================================================================== */
/*  Forward declarations                                               */
/* ================================================================== */

static EastValue *do_diff(EastValue *before, EastValue *after, EastType *type);
static EastValue *do_apply(EastValue *base, EastValue *patch, EastType *type);
static EastValue *do_compose(EastValue *first, EastValue *second, EastType *type);
static EastValue *do_invert(EastValue *patch, EastType *type);

/* ================================================================== */
/*  DIFF: Array (LCS-based)                                            */
/* ================================================================== */

/* Simple LCS for element indices. Returns length of LCS and fills
 * lcs_a[0..ret-1] with indices from a, lcs_b[0..ret-1] from b. */
static size_t compute_lcs(EastValue **a, size_t na, EastValue **b, size_t nb,
                           size_t **out_a, size_t **out_b)
{
    /* DP table */
    int *dp = calloc((na + 1) * (nb + 1), sizeof(int));
    if (!dp) { *out_a = NULL; *out_b = NULL; return 0; }

    for (size_t i = 1; i <= na; i++)
        for (size_t j = 1; j <= nb; j++) {
            if (east_value_equal(a[i-1], b[j-1]))
                dp[i * (nb+1) + j] = dp[(i-1) * (nb+1) + (j-1)] + 1;
            else
                dp[i * (nb+1) + j] = dp[(i-1) * (nb+1) + j] > dp[i * (nb+1) + (j-1)]
                    ? dp[(i-1) * (nb+1) + j] : dp[i * (nb+1) + (j-1)];
        }

    size_t lcs_len = (size_t)dp[na * (nb+1) + nb];
    size_t *la = lcs_len > 0 ? malloc(lcs_len * sizeof(size_t)) : NULL;
    size_t *lb = lcs_len > 0 ? malloc(lcs_len * sizeof(size_t)) : NULL;

    /* Backtrack */
    size_t pos = lcs_len;
    size_t i = na, j = nb;
    while (i > 0 && j > 0 && pos > 0) {
        if (east_value_equal(a[i-1], b[j-1])) {
            pos--;
            la[pos] = i - 1;
            lb[pos] = j - 1;
            i--; j--;
        } else if (dp[(i-1) * (nb+1) + j] > dp[i * (nb+1) + (j-1)]) {
            i--;
        } else {
            j--;
        }
    }

    free(dp);
    *out_a = la;
    *out_b = lb;
    return lcs_len;
}

static EastValue *diff_array(EastValue *before, EastValue *after, EastType *type) {
    if (east_value_equal(before, after)) return mk_unchanged();

    EastType *elem_type = type->data.element;
    size_t na = before->data.array.len;
    size_t nb = after->data.array.len;
    EastValue **a = before->data.array.items;
    EastValue **b = after->data.array.items;

    size_t *lcs_a, *lcs_b;
    size_t lcs_len = compute_lcs(a, na, b, nb, &lcs_a, &lcs_b);

    /* Build operations array */
    EastValue *ops = east_array_new(NULL);
    size_t ai = 0, bi = 0;
    int64_t delete_count = 0, insert_count = 0;

    for (size_t li = 0; li <= lcs_len; li++) {
        size_t match_a = (li < lcs_len) ? lcs_a[li] : na;
        size_t match_b = (li < lcs_len) ? lcs_b[li] : nb;

        /* Deletes before this match */
        while (ai < match_a) {
            int64_t key = (int64_t)ai - delete_count + insert_count;
            const char *op_names[] = {"delete", "insert", "update"};
            EastValue *del_val = a[ai];
            east_value_retain(del_val);
            EastValue *op_vals[] = {del_val, east_null(), east_null()};
            EastValue *op = east_variant_new("delete", del_val, NULL);
            east_value_release(del_val);

            const char *fn[] = {"key", "offset", "operation"};
            EastValue *fv[] = {east_integer(key), east_integer(0), op};
            EastValue *entry = east_struct_new(fn, fv, 3, NULL);
            east_array_push(ops, entry);
            east_value_release(fv[0]);
            east_value_release(fv[1]);
            east_value_release(op);
            east_value_release(entry);
            delete_count++;
            ai++;
        }

        /* Inserts before this match */
        while (bi < match_b) {
            int64_t key = (int64_t)bi;
            EastValue *ins_val = b[bi];
            east_value_retain(ins_val);
            EastValue *op = east_variant_new("insert", ins_val, NULL);
            east_value_release(ins_val);

            const char *fn[] = {"key", "offset", "operation"};
            EastValue *fv[] = {east_integer(key), east_integer(0), op};
            EastValue *entry = east_struct_new(fn, fv, 3, NULL);
            east_array_push(ops, entry);
            east_value_release(fv[0]);
            east_value_release(fv[1]);
            east_value_release(op);
            east_value_release(entry);
            insert_count++;
            bi++;
        }

        /* Skip the matching element */
        if (li < lcs_len) { ai++; bi++; }
    }

    free(lcs_a);
    free(lcs_b);

    if (east_array_len(ops) == 0) {
        east_value_release(ops);
        return mk_unchanged();
    }

    return mk_patch_v(ops);
}

/* ================================================================== */
/*  DIFF: Set                                                          */
/* ================================================================== */

static EastValue *diff_set(EastValue *before, EastValue *after, EastType *type) {
    if (east_value_equal(before, after)) return mk_unchanged();

    EastType *elem_type = type->data.element;

    /* Build dict of operations: key -> Variant{delete: Null, insert: Null} */
    EastValue *ops = east_dict_new(elem_type, NULL);
    size_t del_count = 0, ins_count = 0;

    /* Deletions: elements in before but not in after */
    for (size_t i = 0; i < before->data.set.len; i++) {
        EastValue *elem = before->data.set.items[i];
        if (!east_set_has(after, elem)) {
            EastValue *op = east_variant_new("delete", east_null(), NULL);
            east_dict_set(ops, elem, op);
            east_value_release(op);
            del_count++;
        }
    }

    /* Insertions: elements in after but not in before */
    for (size_t i = 0; i < after->data.set.len; i++) {
        EastValue *elem = after->data.set.items[i];
        if (!east_set_has(before, elem)) {
            EastValue *op = east_variant_new("insert", east_null(), NULL);
            east_dict_set(ops, elem, op);
            east_value_release(op);
            ins_count++;
        }
    }

    /* Optimization: if everything replaced, use replace instead */
    if (del_count == before->data.set.len && ins_count == after->data.set.len &&
        del_count > 0 && ins_count > 0) {
        east_value_release(ops);
        return mk_replace(before, after);
    }

    if (east_dict_len(ops) == 0) {
        east_value_release(ops);
        return mk_unchanged();
    }

    return mk_patch_v(ops);
}

/* ================================================================== */
/*  DIFF: Dict                                                         */
/* ================================================================== */

static EastValue *diff_dict(EastValue *before, EastValue *after, EastType *type) {
    if (east_value_equal(before, after)) return mk_unchanged();

    EastType *key_type = type->data.dict.key;
    EastType *val_type = type->data.dict.value;

    EastValue *ops = east_dict_new(key_type, NULL);
    size_t del_count = 0, ins_count = 0;

    /* Check before keys */
    for (size_t i = 0; i < before->data.dict.len; i++) {
        EastValue *key = before->data.dict.keys[i];
        EastValue *bval = before->data.dict.values[i];
        EastValue *aval = east_dict_get(after, key);
        if (!aval) {
            /* Deleted */
            EastValue *op = east_variant_new("delete", bval, NULL);
            east_dict_set(ops, key, op);
            east_value_release(op);
            del_count++;
        } else if (!east_value_equal(bval, aval)) {
            /* Updated */
            EastValue *vpatch = do_diff(bval, aval, val_type);
            EastValue *op = east_variant_new("update", vpatch, NULL);
            east_dict_set(ops, key, op);
            east_value_release(op);
            east_value_release(vpatch);
        }
    }

    /* Check after keys for insertions */
    for (size_t i = 0; i < after->data.dict.len; i++) {
        EastValue *key = after->data.dict.keys[i];
        if (!east_dict_has(before, key)) {
            EastValue *aval = after->data.dict.values[i];
            EastValue *op = east_variant_new("insert", aval, NULL);
            east_dict_set(ops, key, op);
            east_value_release(op);
            ins_count++;
        }
    }

    /* Optimization: if all deleted and all inserted, use replace */
    if (del_count == before->data.dict.len && ins_count == after->data.dict.len &&
        del_count > 0 && ins_count > 0 &&
        east_dict_len(ops) == del_count + ins_count) {
        east_value_release(ops);
        return mk_replace(before, after);
    }

    if (east_dict_len(ops) == 0) {
        east_value_release(ops);
        return mk_unchanged();
    }

    return mk_patch_v(ops);
}

/* ================================================================== */
/*  DIFF: Struct                                                       */
/* ================================================================== */

static EastValue *diff_struct(EastValue *before, EastValue *after, EastType *type) {
    if (east_value_equal(before, after)) return mk_unchanged();

    size_t nf = type->data.struct_.num_fields;
    const char **names = malloc(nf * sizeof(char *));
    EastValue **patches = malloc(nf * sizeof(EastValue *));
    bool all_unchanged = true;

    for (size_t i = 0; i < nf; i++) {
        names[i] = type->data.struct_.fields[i].name;
        EastType *ft = type->data.struct_.fields[i].type;
        EastValue *bval = before->data.struct_.field_values[i];
        EastValue *aval = after->data.struct_.field_values[i];
        patches[i] = do_diff(bval, aval, ft);
        if (!is_tag(patches[i], "unchanged"))
            all_unchanged = false;
    }

    if (all_unchanged) {
        for (size_t i = 0; i < nf; i++) east_value_release(patches[i]);
        free(names); free(patches);
        return mk_unchanged();
    }

    EastValue *s = east_struct_new(names, patches, nf, NULL);
    for (size_t i = 0; i < nf; i++) east_value_release(patches[i]);
    free(names); free(patches);
    return mk_patch_v(s);
}

/* ================================================================== */
/*  DIFF: Variant                                                      */
/* ================================================================== */

static EastValue *diff_variant(EastValue *before, EastValue *after, EastType *type) {
    if (east_value_equal(before, after)) return mk_unchanged();

    const char *btag = before->data.variant.case_name;
    const char *atag = after->data.variant.case_name;

    if (strcmp(btag, atag) != 0)
        return mk_replace(before, after);

    /* Same case — find the case type and diff values */
    EastType *case_type = NULL;
    for (size_t i = 0; i < type->data.variant.num_cases; i++) {
        if (strcmp(type->data.variant.cases[i].name, btag) == 0) {
            case_type = type->data.variant.cases[i].type;
            break;
        }
    }

    EastValue *vp = do_diff(before->data.variant.value,
                             after->data.variant.value, case_type);
    if (is_tag(vp, "unchanged")) {
        east_value_release(vp);
        return mk_unchanged();
    }

    EastValue *inner = east_variant_new(btag, vp, NULL);
    east_value_release(vp);
    return mk_patch_v(inner);
}

/* ================================================================== */
/*  DIFF: Ref                                                          */
/* ================================================================== */

static EastValue *diff_ref(EastValue *before, EastValue *after, EastType *type) {
    if (before == after) return mk_unchanged();
    EastValue *bv = before->data.ref.value;
    EastValue *av = after->data.ref.value;
    if (east_value_equal(bv, av)) return mk_unchanged();

    EastType *inner_type = type->data.element;
    EastValue *p = do_diff(bv, av, inner_type);
    if (is_tag(p, "unchanged")) {
        east_value_release(p);
        return mk_unchanged();
    }
    return mk_patch_v(p);
}

/* ================================================================== */
/*  DIFF: Main dispatch                                                */
/* ================================================================== */

static EastValue *do_diff(EastValue *before, EastValue *after, EastType *type) {
    bool replace_only;
    EastType *rt = resolve_type(type, &replace_only);

    if (replace_only || !rt) {
        EastValue *result = east_value_equal(before, after)
            ? mk_unchanged() : mk_replace(before, after);
        return result;
    }

    EastValue *result;
    switch (rt->kind) {
    case EAST_TYPE_ARRAY:
        result = diff_array(before, after, rt); break;
    case EAST_TYPE_SET:
        result = diff_set(before, after, rt); break;
    case EAST_TYPE_DICT:
        result = diff_dict(before, after, rt); break;
    case EAST_TYPE_STRUCT:
        result = diff_struct(before, after, rt); break;
    case EAST_TYPE_VARIANT:
        result = diff_variant(before, after, rt); break;
    case EAST_TYPE_REF:
        result = diff_ref(before, after, rt); break;
    default:
        /* Primitives, functions, vectors, matrices — replace only */
        result = east_value_equal(before, after)
            ? mk_unchanged() : mk_replace(before, after);
        break;
    }

    pop_rec_if_pushed(type);
    return result;
}

/* ================================================================== */
/*  APPLY: Array                                                       */
/* ================================================================== */

static EastValue *apply_array(EastValue *base, EastValue *patch_val, EastType *type) {
    /* patch_val is an array of {key, offset, operation} structs */
    EastType *elem_type = type->data.element;

    /* Deep copy base into a mutable array */
    EastValue *result = east_array_new(base->data.array.elem_type);
    for (size_t i = 0; i < base->data.array.len; i++)
        east_array_push(result, base->data.array.items[i]);

    size_t nops = patch_val->data.array.len;
    for (size_t i = 0; i < nops; i++) {
        EastValue *entry = patch_val->data.array.items[i];
        EastValue *key_v = east_struct_get_field(entry, "key");
        EastValue *offset_v = east_struct_get_field(entry, "offset");
        EastValue *op = east_struct_get_field(entry, "operation");
        int64_t idx = key_v->data.integer;
        int64_t offset = offset_v ? offset_v->data.integer : 0;
        int64_t pos = idx + offset;

        const char *op_tag = op->data.variant.case_name;
        if (strcmp(op_tag, "delete") == 0) {
            if (pos >= 0 && (size_t)pos < result->data.array.len) {
                /* Remove element at pos */
                east_value_release(result->data.array.items[pos]);
                memmove(&result->data.array.items[pos],
                        &result->data.array.items[pos + 1],
                        (result->data.array.len - pos - 1) * sizeof(EastValue *));
                result->data.array.len--;
            }
        } else if (strcmp(op_tag, "insert") == 0) {
            EastValue *val = op->data.variant.value;
            /* Insert at pos */
            if (result->data.array.len >= result->data.array.cap) {
                size_t new_cap = result->data.array.cap ? result->data.array.cap * 2 : 4;
                result->data.array.items = realloc(result->data.array.items,
                    new_cap * sizeof(EastValue *));
                result->data.array.cap = new_cap;
            }
            size_t p = (size_t)pos;
            if (p > result->data.array.len) p = result->data.array.len;
            memmove(&result->data.array.items[p + 1],
                    &result->data.array.items[p],
                    (result->data.array.len - p) * sizeof(EastValue *));
            result->data.array.items[p] = val;
            east_value_retain(val);
            result->data.array.len++;
        } else if (strcmp(op_tag, "update") == 0) {
            EastValue *vpatch = op->data.variant.value;
            if (pos >= 0 && (size_t)pos < result->data.array.len) {
                EastValue *old = result->data.array.items[pos];
                EastValue *updated = do_apply(old, vpatch, elem_type);
                east_value_retain(updated);
                east_value_release(old);
                result->data.array.items[pos] = updated;
                east_value_release(updated);
            }
        }
    }
    return result;
}

/* ================================================================== */
/*  APPLY: Set                                                         */
/* ================================================================== */

static EastValue *apply_set(EastValue *base, EastValue *patch_val, EastType *type) {
    /* patch_val is a dict: key -> Variant{delete, insert} */
    EastValue *result = east_set_new(base->data.set.elem_type);

    /* Copy existing elements */
    for (size_t i = 0; i < base->data.set.len; i++)
        east_set_insert(result, base->data.set.items[i]);

    /* Apply operations */
    for (size_t i = 0; i < patch_val->data.dict.len; i++) {
        EastValue *key = patch_val->data.dict.keys[i];
        EastValue *op = patch_val->data.dict.values[i];
        const char *tag = op->data.variant.case_name;
        if (strcmp(tag, "delete") == 0) {
            /* Remove key from result set */
            for (size_t j = 0; j < result->data.set.len; j++) {
                if (east_value_equal(result->data.set.items[j], key)) {
                    east_value_release(result->data.set.items[j]);
                    memmove(&result->data.set.items[j],
                            &result->data.set.items[j + 1],
                            (result->data.set.len - j - 1) * sizeof(EastValue *));
                    result->data.set.len--;
                    break;
                }
            }
        } else if (strcmp(tag, "insert") == 0) {
            east_set_insert(result, key);
        }
    }
    return result;
}

/* ================================================================== */
/*  APPLY: Dict                                                        */
/* ================================================================== */

static EastValue *apply_dict(EastValue *base, EastValue *patch_val, EastType *type) {
    EastType *val_type = type->data.dict.value;

    /* Deep copy base */
    EastValue *result = east_dict_new(base->data.dict.key_type,
                                       base->data.dict.val_type);
    for (size_t i = 0; i < base->data.dict.len; i++)
        east_dict_set(result, base->data.dict.keys[i], base->data.dict.values[i]);

    /* Apply operations */
    for (size_t i = 0; i < patch_val->data.dict.len; i++) {
        EastValue *key = patch_val->data.dict.keys[i];
        EastValue *op = patch_val->data.dict.values[i];
        const char *tag = op->data.variant.case_name;

        if (strcmp(tag, "delete") == 0) {
            /* Remove key — rebuild without it */
            EastValue *new_result = east_dict_new(result->data.dict.key_type,
                                                   result->data.dict.val_type);
            for (size_t j = 0; j < result->data.dict.len; j++) {
                if (!east_value_equal(result->data.dict.keys[j], key))
                    east_dict_set(new_result, result->data.dict.keys[j],
                                  result->data.dict.values[j]);
            }
            east_value_release(result);
            result = new_result;
        } else if (strcmp(tag, "insert") == 0) {
            EastValue *val = op->data.variant.value;
            east_dict_set(result, key, val);
        } else if (strcmp(tag, "update") == 0) {
            EastValue *vpatch = op->data.variant.value;
            EastValue *old = east_dict_get(result, key);
            if (old) {
                EastValue *updated = do_apply(old, vpatch, val_type);
                east_dict_set(result, key, updated);
                east_value_release(updated);
            }
        }
    }
    return result;
}

/* ================================================================== */
/*  APPLY: Struct                                                      */
/* ================================================================== */

static EastValue *apply_struct(EastValue *base, EastValue *patch_val, EastType *type) {
    size_t nf = type->data.struct_.num_fields;
    const char **names = malloc(nf * sizeof(char *));
    EastValue **vals = malloc(nf * sizeof(EastValue *));

    for (size_t i = 0; i < nf; i++) {
        names[i] = type->data.struct_.fields[i].name;
        EastType *ft = type->data.struct_.fields[i].type;
        EastValue *bval = base->data.struct_.field_values[i];
        EastValue *fp = east_struct_get_field(patch_val, names[i]);
        vals[i] = do_apply(bval, fp, ft);
    }

    EastValue *result = east_struct_new(names, vals, nf, NULL);
    for (size_t i = 0; i < nf; i++) east_value_release(vals[i]);
    free(names); free(vals);
    return result;
}

/* ================================================================== */
/*  APPLY: Variant                                                     */
/* ================================================================== */

static EastValue *apply_variant(EastValue *base, EastValue *patch_val, EastType *type) {
    /* patch_val is a variant(caseName, casePatch) */
    const char *case_name = patch_val->data.variant.case_name;
    EastValue *case_patch = patch_val->data.variant.value;

    /* Find case type */
    EastType *case_type = NULL;
    for (size_t i = 0; i < type->data.variant.num_cases; i++) {
        if (strcmp(type->data.variant.cases[i].name, case_name) == 0) {
            case_type = type->data.variant.cases[i].type;
            break;
        }
    }

    EastValue *new_val = do_apply(base->data.variant.value, case_patch, case_type);
    EastValue *result = east_variant_new(case_name, new_val, NULL);
    east_value_release(new_val);
    return result;
}

/* ================================================================== */
/*  APPLY: Ref                                                         */
/* ================================================================== */

static EastValue *apply_ref(EastValue *base, EastValue *patch_val, EastType *type) {
    EastType *inner_type = type->data.element;
    EastValue *old = base->data.ref.value;
    EastValue *updated = do_apply(old, patch_val, inner_type);
    EastValue *result = east_ref_new(updated);
    east_value_release(updated);
    return result;
}

/* ================================================================== */
/*  APPLY: Main dispatch                                               */
/* ================================================================== */

static EastValue *do_apply(EastValue *base, EastValue *patch, EastType *type) {
    if (!patch || !base) {
        east_value_retain(base);
        return base;
    }

    if (is_tag(patch, "unchanged")) {
        east_value_retain(base);
        return base;
    }

    if (is_tag(patch, "replace")) {
        EastValue *after = replace_after(patch);
        east_value_retain(after);
        return after;
    }

    if (!is_tag(patch, "patch")) {
        east_value_retain(base);
        return base;
    }

    EastValue *patch_val = patch_payload(patch);
    bool replace_only;
    EastType *rt = resolve_type(type, &replace_only);

    EastValue *result;
    if (replace_only || !rt) {
        east_value_retain(base);
        result = base;
    } else {
        switch (rt->kind) {
        case EAST_TYPE_ARRAY:
            result = apply_array(base, patch_val, rt); break;
        case EAST_TYPE_SET:
            result = apply_set(base, patch_val, rt); break;
        case EAST_TYPE_DICT:
            result = apply_dict(base, patch_val, rt); break;
        case EAST_TYPE_STRUCT:
            result = apply_struct(base, patch_val, rt); break;
        case EAST_TYPE_VARIANT:
            result = apply_variant(base, patch_val, rt); break;
        case EAST_TYPE_REF:
            result = apply_ref(base, patch_val, rt); break;
        default:
            east_value_retain(base);
            result = base;
            break;
        }
    }

    pop_rec_if_pushed(type);
    return result;
}

/* ================================================================== */
/*  COMPOSE helpers                                                    */
/* ================================================================== */

static EastValue *compose_struct(EastValue *first, EastValue *second, EastType *type) {
    size_t nf = type->data.struct_.num_fields;
    const char **names = malloc(nf * sizeof(char *));
    EastValue **vals = malloc(nf * sizeof(EastValue *));
    bool all_unchanged = true;

    for (size_t i = 0; i < nf; i++) {
        names[i] = type->data.struct_.fields[i].name;
        EastType *ft = type->data.struct_.fields[i].type;
        EastValue *fp1 = east_struct_get_field(first, names[i]);
        EastValue *fp2 = east_struct_get_field(second, names[i]);
        vals[i] = do_compose(fp1, fp2, ft);
        if (!is_tag(vals[i], "unchanged")) all_unchanged = false;
    }

    if (all_unchanged) {
        for (size_t i = 0; i < nf; i++) east_value_release(vals[i]);
        free(names); free(vals);
        return mk_unchanged();
    }

    EastValue *s = east_struct_new(names, vals, nf, NULL);
    for (size_t i = 0; i < nf; i++) east_value_release(vals[i]);
    free(names); free(vals);
    return mk_patch_v(s);
}

static EastValue *compose_variant(EastValue *first, EastValue *second, EastType *type) {
    const char *c1 = first->data.variant.case_name;
    const char *c2 = second->data.variant.case_name;
    if (strcmp(c1, c2) != 0) {
        /* Different cases — can't compose structurally */
        east_builtin_error("Cannot compose patches for different variant cases");
        return NULL;
    }

    EastType *case_type = NULL;
    for (size_t i = 0; i < type->data.variant.num_cases; i++) {
        if (strcmp(type->data.variant.cases[i].name, c1) == 0) {
            case_type = type->data.variant.cases[i].type;
            break;
        }
    }

    EastValue *composed = do_compose(first->data.variant.value,
                                      second->data.variant.value, case_type);
    if (is_tag(composed, "unchanged")) {
        east_value_release(composed);
        return mk_unchanged();
    }

    EastValue *inner = east_variant_new(c1, composed, NULL);
    east_value_release(composed);
    return mk_patch_v(inner);
}

static EastValue *compose_ref(EastValue *first, EastValue *second, EastType *type) {
    EastType *inner_type = type->data.element;
    EastValue *composed = do_compose(first, second, inner_type);
    if (is_tag(composed, "unchanged")) {
        east_value_release(composed);
        return mk_unchanged();
    }
    return mk_patch_v(composed);
}

static EastValue *compose_set(EastValue *first, EastValue *second, EastType *type) {
    /* Merge operations by key */
    EastType *elem_type = type->data.element;
    EastValue *result = east_dict_new(elem_type, NULL);

    /* Add all from first */
    for (size_t i = 0; i < first->data.dict.len; i++) {
        EastValue *key = first->data.dict.keys[i];
        EastValue *op1 = first->data.dict.values[i];
        EastValue *op2 = east_dict_get(second, key);
        if (op2) {
            /* Both have this key — cancel out (insert+delete or delete+insert) */
            /* Don't add to result */
        } else {
            east_dict_set(result, key, op1);
        }
    }

    /* Add from second that aren't in first */
    for (size_t i = 0; i < second->data.dict.len; i++) {
        EastValue *key = second->data.dict.keys[i];
        if (!east_dict_has(first, key))
            east_dict_set(result, key, second->data.dict.values[i]);
    }

    if (east_dict_len(result) == 0) {
        east_value_release(result);
        return mk_unchanged();
    }
    return mk_patch_v(result);
}

static EastValue *compose_dict(EastValue *first, EastValue *second, EastType *type) {
    EastType *key_type = type->data.dict.key;
    EastType *val_type = type->data.dict.value;
    EastValue *result = east_dict_new(key_type, NULL);

    /* Process first dict */
    for (size_t i = 0; i < first->data.dict.len; i++) {
        EastValue *key = first->data.dict.keys[i];
        EastValue *op1 = first->data.dict.values[i];
        EastValue *op2 = east_dict_get(second, key);
        if (!op2) {
            east_dict_set(result, key, op1);
            continue;
        }

        const char *t1 = op1->data.variant.case_name;
        const char *t2 = op2->data.variant.case_name;

        if (strcmp(t1, "insert") == 0 && strcmp(t2, "delete") == 0) {
            /* Cancel out — don't add */
        } else if (strcmp(t1, "insert") == 0 && strcmp(t2, "update") == 0) {
            /* Apply update to inserted value */
            EastValue *new_val = do_apply(op1->data.variant.value,
                                           op2->data.variant.value, val_type);
            EastValue *new_op = east_variant_new("insert", new_val, NULL);
            east_dict_set(result, key, new_op);
            east_value_release(new_op);
            east_value_release(new_val);
        } else if (strcmp(t1, "delete") == 0 && strcmp(t2, "insert") == 0) {
            /* Delete then insert = update(replace(old, new)) */
            EastValue *rp = mk_replace(op1->data.variant.value,
                                        op2->data.variant.value);
            EastValue *new_op = east_variant_new("update", rp, NULL);
            east_dict_set(result, key, new_op);
            east_value_release(new_op);
            east_value_release(rp);
        } else if (strcmp(t1, "update") == 0 && strcmp(t2, "update") == 0) {
            /* Compose the updates */
            EastValue *composed = do_compose(op1->data.variant.value,
                                              op2->data.variant.value, val_type);
            EastValue *new_op = east_variant_new("update", composed, NULL);
            east_dict_set(result, key, new_op);
            east_value_release(new_op);
            east_value_release(composed);
        } else {
            east_dict_set(result, key, op1);
        }
    }

    /* Add from second that aren't in first */
    for (size_t i = 0; i < second->data.dict.len; i++) {
        EastValue *key = second->data.dict.keys[i];
        if (!east_dict_has(first, key))
            east_dict_set(result, key, second->data.dict.values[i]);
    }

    if (east_dict_len(result) == 0) {
        east_value_release(result);
        return mk_unchanged();
    }
    return mk_patch_v(result);
}

static EastValue *compose_array(EastValue *first, EastValue *second, EastType *type) {
    /* Concatenate operations */
    EastValue *result = east_array_new(NULL);
    for (size_t i = 0; i < first->data.array.len; i++)
        east_array_push(result, first->data.array.items[i]);
    for (size_t i = 0; i < second->data.array.len; i++)
        east_array_push(result, second->data.array.items[i]);

    if (east_array_len(result) == 0) {
        east_value_release(result);
        return mk_unchanged();
    }
    return mk_patch_v(result);
}

/* ================================================================== */
/*  COMPOSE: Main dispatch                                             */
/* ================================================================== */

static EastValue *do_compose(EastValue *first, EastValue *second, EastType *type) {
    if (!first || !second) return mk_unchanged();

    /* unchanged + X = X, X + unchanged = X */
    if (is_tag(first, "unchanged")) {
        east_value_retain(second);
        return second;
    }
    if (is_tag(second, "unchanged")) {
        east_value_retain(first);
        return first;
    }

    /* replace + replace = replace(first.before, second.after) */
    if (is_tag(first, "replace") && is_tag(second, "replace")) {
        return mk_replace(replace_before(first), replace_after(second));
    }

    /* replace + patch = replace(first.before, apply(first.after, second)) */
    if (is_tag(first, "replace") && is_tag(second, "patch")) {
        EastValue *applied = do_apply(replace_after(first), second, type);
        EastValue *result = mk_replace(replace_before(first), applied);
        east_value_release(applied);
        return result;
    }

    /* patch + replace = replace(apply_inverse(second.before), second.after)
     * Simplified: just compute replace(invert_apply(first, second.before), second.after)
     * Actually: we invert first, apply to second.before to get original before */
    if (is_tag(first, "patch") && is_tag(second, "replace")) {
        EastValue *inv = do_invert(first, type);
        EastValue *original = do_apply(replace_before(second), inv, type);
        EastValue *result = mk_replace(original, replace_after(second));
        east_value_release(inv);
        east_value_release(original);
        return result;
    }

    /* patch + patch: type-specific compose */
    if (is_tag(first, "patch") && is_tag(second, "patch")) {
        bool replace_only;
        EastType *rt = resolve_type(type, &replace_only);
        EastValue *result;

        if (replace_only || !rt) {
            result = mk_unchanged();
        } else {
            EastValue *p1 = patch_payload(first);
            EastValue *p2 = patch_payload(second);
            switch (rt->kind) {
            case EAST_TYPE_ARRAY:
                result = compose_array(p1, p2, rt); break;
            case EAST_TYPE_SET:
                result = compose_set(p1, p2, rt); break;
            case EAST_TYPE_DICT:
                result = compose_dict(p1, p2, rt); break;
            case EAST_TYPE_STRUCT:
                result = compose_struct(p1, p2, rt); break;
            case EAST_TYPE_VARIANT:
                result = compose_variant(p1, p2, rt); break;
            case EAST_TYPE_REF:
                result = compose_ref(p1, p2, rt); break;
            default:
                result = mk_unchanged(); break;
            }
        }
        pop_rec_if_pushed(type);
        return result;
    }

    return mk_unchanged();
}

/* ================================================================== */
/*  INVERT helpers                                                     */
/* ================================================================== */

static EastValue *invert_array(EastValue *patch_val, EastType *type) {
    EastType *elem_type = type->data.element;
    size_t n = patch_val->data.array.len;
    EastValue *result = east_array_new(NULL);

    /* Reverse order and invert each operation */
    for (size_t i = n; i > 0; i--) {
        EastValue *entry = patch_val->data.array.items[i - 1];
        EastValue *key_v = east_struct_get_field(entry, "key");
        EastValue *offset_v = east_struct_get_field(entry, "offset");
        EastValue *op = east_struct_get_field(entry, "operation");
        const char *tag = op->data.variant.case_name;

        EastValue *new_op;
        if (strcmp(tag, "delete") == 0) {
            new_op = east_variant_new("insert", op->data.variant.value, NULL);
        } else if (strcmp(tag, "insert") == 0) {
            new_op = east_variant_new("delete", op->data.variant.value, NULL);
        } else if (strcmp(tag, "update") == 0) {
            EastValue *inv = do_invert(op->data.variant.value, elem_type);
            new_op = east_variant_new("update", inv, NULL);
            east_value_release(inv);
        } else {
            new_op = east_variant_new(tag, op->data.variant.value, NULL);
        }

        const char *fn[] = {"key", "offset", "operation"};
        EastValue *fv[] = {key_v, offset_v, new_op};
        EastValue *new_entry = east_struct_new(fn, fv, 3, NULL);
        east_array_push(result, new_entry);
        east_value_release(new_op);
        east_value_release(new_entry);
    }
    return mk_patch_v(result);
}

static EastValue *invert_set(EastValue *patch_val, EastType *type) {
    EastType *elem_type = type->data.element;
    EastValue *result = east_dict_new(elem_type, NULL);

    for (size_t i = 0; i < patch_val->data.dict.len; i++) {
        EastValue *key = patch_val->data.dict.keys[i];
        EastValue *op = patch_val->data.dict.values[i];
        const char *tag = op->data.variant.case_name;
        EastValue *new_op;
        if (strcmp(tag, "delete") == 0) {
            new_op = east_variant_new("insert", east_null(), NULL);
        } else if (strcmp(tag, "insert") == 0) {
            new_op = east_variant_new("delete", east_null(), NULL);
        } else {
            new_op = east_variant_new(tag, op->data.variant.value, NULL);
        }
        east_dict_set(result, key, new_op);
        east_value_release(new_op);
    }

    if (east_dict_len(result) == 0) {
        east_value_release(result);
        return mk_unchanged();
    }
    return mk_patch_v(result);
}

static EastValue *invert_dict(EastValue *patch_val, EastType *type) {
    EastType *key_type = type->data.dict.key;
    EastType *val_type = type->data.dict.value;
    EastValue *result = east_dict_new(key_type, NULL);

    for (size_t i = 0; i < patch_val->data.dict.len; i++) {
        EastValue *key = patch_val->data.dict.keys[i];
        EastValue *op = patch_val->data.dict.values[i];
        const char *tag = op->data.variant.case_name;
        EastValue *new_op;
        if (strcmp(tag, "delete") == 0) {
            new_op = east_variant_new("insert", op->data.variant.value, NULL);
        } else if (strcmp(tag, "insert") == 0) {
            new_op = east_variant_new("delete", op->data.variant.value, NULL);
        } else if (strcmp(tag, "update") == 0) {
            EastValue *inv = do_invert(op->data.variant.value, val_type);
            new_op = east_variant_new("update", inv, NULL);
            east_value_release(inv);
        } else {
            new_op = east_variant_new(tag, op->data.variant.value, NULL);
        }
        east_dict_set(result, key, new_op);
        east_value_release(new_op);
    }

    if (east_dict_len(result) == 0) {
        east_value_release(result);
        return mk_unchanged();
    }
    return mk_patch_v(result);
}

static EastValue *invert_struct(EastValue *patch_val, EastType *type) {
    size_t nf = type->data.struct_.num_fields;
    const char **names = malloc(nf * sizeof(char *));
    EastValue **vals = malloc(nf * sizeof(EastValue *));
    bool all_unchanged = true;

    for (size_t i = 0; i < nf; i++) {
        names[i] = type->data.struct_.fields[i].name;
        EastType *ft = type->data.struct_.fields[i].type;
        EastValue *fp = east_struct_get_field(patch_val, names[i]);
        vals[i] = do_invert(fp, ft);
        if (!is_tag(vals[i], "unchanged")) all_unchanged = false;
    }

    if (all_unchanged) {
        for (size_t i = 0; i < nf; i++) east_value_release(vals[i]);
        free(names); free(vals);
        return mk_unchanged();
    }

    EastValue *s = east_struct_new(names, vals, nf, NULL);
    for (size_t i = 0; i < nf; i++) east_value_release(vals[i]);
    free(names); free(vals);
    return mk_patch_v(s);
}

static EastValue *invert_variant(EastValue *patch_val, EastType *type) {
    const char *case_name = patch_val->data.variant.case_name;
    EastType *case_type = NULL;
    for (size_t i = 0; i < type->data.variant.num_cases; i++) {
        if (strcmp(type->data.variant.cases[i].name, case_name) == 0) {
            case_type = type->data.variant.cases[i].type;
            break;
        }
    }

    EastValue *inv = do_invert(patch_val->data.variant.value, case_type);
    if (is_tag(inv, "unchanged")) {
        east_value_release(inv);
        return mk_unchanged();
    }

    EastValue *inner = east_variant_new(case_name, inv, NULL);
    east_value_release(inv);
    return mk_patch_v(inner);
}

static EastValue *invert_ref(EastValue *patch_val, EastType *type) {
    EastType *inner_type = type->data.element;
    EastValue *inv = do_invert(patch_val, inner_type);
    if (is_tag(inv, "unchanged")) {
        east_value_release(inv);
        return mk_unchanged();
    }
    return mk_patch_v(inv);
}

/* ================================================================== */
/*  INVERT: Main dispatch                                              */
/* ================================================================== */

static EastValue *do_invert(EastValue *patch, EastType *type) {
    if (!patch) return mk_unchanged();

    if (is_tag(patch, "unchanged"))
        return mk_unchanged();

    if (is_tag(patch, "replace"))
        return mk_replace(replace_after(patch), replace_before(patch));

    if (!is_tag(patch, "patch"))
        return mk_unchanged();

    EastValue *patch_val = patch_payload(patch);
    bool replace_only;
    EastType *rt = resolve_type(type, &replace_only);

    EastValue *result;
    if (replace_only || !rt) {
        result = mk_unchanged();
    } else {
        switch (rt->kind) {
        case EAST_TYPE_ARRAY:
            result = invert_array(patch_val, rt); break;
        case EAST_TYPE_SET:
            result = invert_set(patch_val, rt); break;
        case EAST_TYPE_DICT:
            result = invert_dict(patch_val, rt); break;
        case EAST_TYPE_STRUCT:
            result = invert_struct(patch_val, rt); break;
        case EAST_TYPE_VARIANT:
            result = invert_variant(patch_val, rt); break;
        case EAST_TYPE_REF:
            result = invert_ref(patch_val, rt); break;
        default:
            result = mk_unchanged(); break;
        }
    }

    pop_rec_if_pushed(type);
    return result;
}

/* ================================================================== */
/*  Top-level implementations                                          */
/* ================================================================== */

static EastValue *patch_diff_impl(EastValue **args, size_t n) {
    (void)n;
    rec_depth = 0;
    return do_diff(args[0], args[1], s_patch_type);
}

static EastValue *patch_apply_impl(EastValue **args, size_t n) {
    (void)n;
    rec_depth = 0;
    return do_apply(args[0], args[1], s_patch_type);
}

static EastValue *patch_compose_impl(EastValue **args, size_t n) {
    (void)n;
    rec_depth = 0;
    return do_compose(args[0], args[1], s_patch_type);
}

static EastValue *patch_invert_impl(EastValue **args, size_t n) {
    (void)n;
    rec_depth = 0;
    return do_invert(args[0], s_patch_type);
}

/* ================================================================== */
/*  Factory functions                                                  */
/* ================================================================== */

static BuiltinImpl diff_factory(EastType **tp, size_t ntp) {
    s_patch_type = (ntp > 0) ? tp[0] : NULL;
    return patch_diff_impl;
}

static BuiltinImpl apply_patch_factory(EastType **tp, size_t ntp) {
    s_patch_type = (ntp > 0) ? tp[0] : NULL;
    return patch_apply_impl;
}

static BuiltinImpl compose_patch_factory(EastType **tp, size_t ntp) {
    s_patch_type = (ntp > 0) ? tp[0] : NULL;
    return patch_compose_impl;
}

static BuiltinImpl invert_patch_factory(EastType **tp, size_t ntp) {
    s_patch_type = (ntp > 0) ? tp[0] : NULL;
    return patch_invert_impl;
}

/* ================================================================== */
/*  Registration                                                       */
/* ================================================================== */

void east_register_patch_builtins(BuiltinRegistry *reg) {
    builtin_registry_register(reg, "Diff", diff_factory);
    builtin_registry_register(reg, "ApplyPatch", apply_patch_factory);
    builtin_registry_register(reg, "ComposePatch", compose_patch_factory);
    builtin_registry_register(reg, "InvertPatch", invert_patch_factory);
}
