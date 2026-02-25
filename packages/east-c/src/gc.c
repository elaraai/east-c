#include "east/gc.h"
#include "east/values.h"
#include "east/compiler.h"
#include "east/env.h"
#include "east/hashmap.h"
#include "east/types.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  GC tracking list (circular doubly-linked with sentinel)            */
/* ------------------------------------------------------------------ */

/* _Thread_local can't have self-referencing initializers, so we
 * lazy-init the sentinel's next/prev pointers on first use. */
static _Thread_local EastValue gc_sentinel = {
    .kind = EAST_VAL_NULL,
    .ref_count = -1,
    .gc_next = NULL,
    .gc_prev = NULL,
    .gc_tracked = false,
};

static _Thread_local size_t gc_count = 0;
static _Thread_local unsigned gc_generation = 0;

static inline void gc_ensure_init(void) {
    if (!gc_sentinel.gc_next) {
        gc_sentinel.gc_next = &gc_sentinel;
        gc_sentinel.gc_prev = &gc_sentinel;
    }
}

void east_gc_track(EastValue *v) {
    if (!v || v->gc_tracked) return;
    gc_ensure_init();
    v->gc_next = gc_sentinel.gc_next;
    v->gc_prev = &gc_sentinel;
    gc_sentinel.gc_next->gc_prev = v;
    gc_sentinel.gc_next = v;
    v->gc_tracked = true;
    gc_count++;
}

void east_gc_untrack(EastValue *v) {
    if (!v || !v->gc_tracked) return;
    v->gc_prev->gc_next = v->gc_next;
    v->gc_next->gc_prev = v->gc_prev;
    v->gc_next = NULL;
    v->gc_prev = NULL;
    v->gc_tracked = false;
    gc_count--;
}

size_t east_gc_tracked_count(void) {
    return gc_count;
}

/* ------------------------------------------------------------------ */
/*  Traverse: visit all EastValue* references held by a value          */
/* ------------------------------------------------------------------ */

typedef void (*gc_visit_fn)(EastValue *child, void *ctx);

/* hashmap_iter callback that visits each value in an environment */
typedef struct {
    gc_visit_fn visit;
    void *ctx;
} EnvVisitCtx;

static void env_visit_cb(const char *key, void *value, void *ctx) {
    (void)key;
    EnvVisitCtx *ectx = (EnvVisitCtx *)ctx;
    if (value) ectx->visit((EastValue *)value, ectx->ctx);
}

static void gc_traverse(EastValue *v, gc_visit_fn visit, void *ctx) {
    switch (v->kind) {
    case EAST_VAL_ARRAY:
        for (size_t i = 0; i < v->data.array.len; i++) {
            if (v->data.array.items[i])
                visit(v->data.array.items[i], ctx);
        }
        break;

    case EAST_VAL_SET:
        for (size_t i = 0; i < v->data.set.len; i++) {
            if (v->data.set.items[i])
                visit(v->data.set.items[i], ctx);
        }
        break;

    case EAST_VAL_DICT:
        for (size_t i = 0; i < v->data.dict.len; i++) {
            if (v->data.dict.keys[i])
                visit(v->data.dict.keys[i], ctx);
            if (v->data.dict.values[i])
                visit(v->data.dict.values[i], ctx);
        }
        break;

    case EAST_VAL_STRUCT:
        for (size_t i = 0; i < v->data.struct_.num_fields; i++) {
            if (v->data.struct_.field_values[i])
                visit(v->data.struct_.field_values[i], ctx);
        }
        break;

    case EAST_VAL_VARIANT:
        if (v->data.variant.value)
            visit(v->data.variant.value, ctx);
        break;

    case EAST_VAL_REF:
        if (v->data.ref.value)
            visit(v->data.ref.value, ctx);
        break;

    case EAST_VAL_FUNCTION:
        if (v->data.function.compiled &&
            v->data.function.compiled->captures) {
            EnvVisitCtx ectx = { .visit = visit, .ctx = ctx };
            for (Environment *env = v->data.function.compiled->captures;
                 env != NULL; env = env->parent) {
                if (env->gc_gen == gc_generation) break;
                env->gc_gen = gc_generation;
                if (env->locals)
                    hashmap_iter(env->locals, env_visit_cb, &ectx);
            }
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Cycle collector: trial-deletion algorithm                          */
/* ------------------------------------------------------------------ */

/*
 * Phase 1: copy ref_count → gc_refs for all tracked objects
 * Phase 2: for each tracked object, traverse its references and
 *          decrement gc_refs of tracked children (trial deletion)
 * Phase 3: objects with gc_refs > 0 are roots; rescue all objects
 *          transitively reachable from roots
 * Phase 4: remaining objects with gc_refs == 0 are garbage — collect
 */

/* Phase 2 visitor: decrement gc_refs of tracked children */
static void subtract_ref(EastValue *child, void *ctx) {
    (void)ctx;
    if (child && child->gc_tracked) {
        child->gc_refs--;
    }
}

/* Phase 3 visitor: rescue tentatively unreachable objects */
static void rescue_visit(EastValue *child, void *ctx) {
    (void)ctx;
    if (child && child->gc_tracked && child->gc_refs == 0) {
        child->gc_refs = 1; /* mark as rescued */
        gc_traverse(child, rescue_visit, NULL);
    }
}

/* Phase 4: destroy contents of a garbage value without triggering
 * cascading frees into other garbage objects (their ref_count has
 * been set to INT_MAX so east_value_release won't free them). */
static void gc_destroy_contents(EastValue *v) {
    switch (v->kind) {
    case EAST_VAL_ARRAY:
        for (size_t i = 0; i < v->data.array.len; i++)
            east_value_release(v->data.array.items[i]);
        free(v->data.array.items);
        if (v->data.array.elem_type)
            east_type_release(v->data.array.elem_type);
        v->data.array.items = NULL;
        v->data.array.len = 0;
        break;

    case EAST_VAL_SET:
        for (size_t i = 0; i < v->data.set.len; i++)
            east_value_release(v->data.set.items[i]);
        free(v->data.set.items);
        if (v->data.set.elem_type)
            east_type_release(v->data.set.elem_type);
        v->data.set.items = NULL;
        v->data.set.len = 0;
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
        v->data.dict.keys = NULL;
        v->data.dict.values = NULL;
        v->data.dict.len = 0;
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
        v->data.struct_.field_names = NULL;
        v->data.struct_.field_values = NULL;
        v->data.struct_.num_fields = 0;
        break;

    case EAST_VAL_VARIANT:
        free(v->data.variant.case_name);
        east_value_release(v->data.variant.value);
        if (v->data.variant.type)
            east_type_release(v->data.variant.type);
        v->data.variant.case_name = NULL;
        v->data.variant.value = NULL;
        break;

    case EAST_VAL_REF:
        east_value_release(v->data.ref.value);
        v->data.ref.value = NULL;
        break;

    case EAST_VAL_FUNCTION:
        if (v->data.function.compiled) {
            east_compiled_fn_free(v->data.function.compiled);
            v->data.function.compiled = NULL;
        }
        break;

    default:
        break;
    }
}

void east_gc_collect(void) {
    gc_ensure_init();
    if (gc_count == 0) return;

    /* Phase 1: copy refcounts */
    for (EastValue *v = gc_sentinel.gc_next; v != &gc_sentinel;
         v = v->gc_next) {
        v->gc_refs = v->ref_count;
    }

    /* Phase 2: subtract internal references (trial deletion)
     * Increment gc_generation so each shared env is visited only once. */
    gc_generation++;
    for (EastValue *v = gc_sentinel.gc_next; v != &gc_sentinel;
         v = v->gc_next) {
        gc_traverse(v, subtract_ref, NULL);
    }

    /* Phase 3: rescue objects reachable from roots (gc_refs > 0)
     * New generation so envs are re-traversable for rescue. */
    gc_generation++;
    for (EastValue *v = gc_sentinel.gc_next; v != &gc_sentinel;
         v = v->gc_next) {
        if (v->gc_refs > 0) {
            gc_traverse(v, rescue_visit, NULL);
        }
    }

    /* Phase 4: collect garbage (gc_refs == 0 after rescue) */

    /* 4a: Build garbage list and untrack.  Set ref_count to INT_MAX
     *     so that east_value_release called during gc_destroy_contents
     *     on other garbage objects will not trigger their deallocation. */
    size_t garbage_cap = 64;
    size_t garbage_len = 0;
    EastValue **garbage = malloc(garbage_cap * sizeof(EastValue *));
    if (!garbage) return; /* OOM — skip collection */

    EastValue *v = gc_sentinel.gc_next;
    while (v != &gc_sentinel) {
        EastValue *next = v->gc_next;
        if (v->gc_refs == 0) {
            /* Unlink from tracking list */
            v->gc_prev->gc_next = v->gc_next;
            v->gc_next->gc_prev = v->gc_prev;
            v->gc_next = NULL;
            v->gc_prev = NULL;
            v->gc_tracked = false;
            gc_count--;

            v->ref_count = INT_MAX;

            if (garbage_len >= garbage_cap) {
                garbage_cap *= 2;
                EastValue **ng = realloc(garbage,
                                         garbage_cap * sizeof(EastValue *));
                if (!ng) { free(garbage); return; }
                garbage = ng;
            }
            garbage[garbage_len++] = v;
        }
        v = next;
    }

    /* 4b: Destroy contents of each garbage object (breaks cycles) */
    for (size_t i = 0; i < garbage_len; i++) {
        gc_destroy_contents(garbage[i]);
    }

    /* 4c: Free the garbage objects themselves */
    for (size_t i = 0; i < garbage_len; i++) {
        free(garbage[i]);
    }

    free(garbage);
}
