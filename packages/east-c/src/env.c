#include "east/env.h"
#include "east/arena.h"

#include <stdlib.h>

/* Callback passed to hashmap_free to release each stored EastValue. */
static void release_value_cb(void *v) {
    if (v) east_value_release((EastValue *)v);
}

Environment *env_new(Environment *parent) {
    Environment *env = east_alloc(sizeof(Environment));
    if (!env) return NULL;
    env->locals = hashmap_new();
    if (!env->locals) {
        east_free(env);
        return NULL;
    }
    env->parent = parent;
    if (parent) env_retain(parent);
    env->ref_count = 1;
    env->gc_gen = 0;
    return env;
}

void env_set(Environment *env, const char *name, EastValue *value) {
    if (!env || !name) return;

    /* If the key already exists, release the old value before overwriting. */
    EastValue *old = (EastValue *)hashmap_get(env->locals, name);
    if (old) {
        east_value_release(old);
    }

    if (value) east_value_retain(value);
    hashmap_set(env->locals, name, value);
}

void env_update(Environment *env, const char *name, EastValue *value) {
    if (!env || !name) return;

    /* Walk scope chain to find existing binding and update it. */
    for (Environment *cur = env; cur != NULL; cur = cur->parent) {
        if (hashmap_has(cur->locals, name)) {
            EastValue *old = (EastValue *)hashmap_get(cur->locals, name);
            if (old) east_value_release(old);
            if (value) east_value_retain(value);
            hashmap_set(cur->locals, name, value);
            return;
        }
    }

    /* Fallback: create new binding in current scope. */
    if (value) east_value_retain(value);
    hashmap_set(env->locals, name, value);
}

EastValue *env_get(Environment *env, const char *name) {
    if (!env || !name) return NULL;

    for (Environment *cur = env; cur != NULL; cur = cur->parent) {
        EastValue *val = (EastValue *)hashmap_get(cur->locals, name);
        if (val) return val;
    }
    return NULL;
}

bool env_has(Environment *env, const char *name) {
    if (!env || !name) return false;

    for (Environment *cur = env; cur != NULL; cur = cur->parent) {
        if (hashmap_has(cur->locals, name)) return true;
    }
    return false;
}

void env_retain(Environment *env) {
    if (env) env->ref_count++;
}

void env_release(Environment *env) {
    if (!env) return;
    if (--env->ref_count > 0) return;

    /* Release all values stored in the locals hashmap, free keys, free map. */
    hashmap_free(env->locals, release_value_cb);

    /* Release the parent environment. */
    if (env->parent) env_release(env->parent);

    free(env);
}
