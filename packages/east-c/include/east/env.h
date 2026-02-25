#ifndef EAST_ENV_H
#define EAST_ENV_H

#include "hashmap.h"
#include "values.h"
#include <stdbool.h>

typedef struct Environment {
    Hashmap *locals;
    struct Environment *parent;
    int ref_count;
    unsigned gc_gen;  /* generation stamp for GC dedup */
} Environment;

Environment *env_new(Environment *parent);
void env_set(Environment *env, const char *name, EastValue *value);
void env_update(Environment *env, const char *name, EastValue *value);
EastValue *env_get(Environment *env, const char *name);
bool env_has(Environment *env, const char *name);
void env_retain(Environment *env);
void env_release(Environment *env);

#endif
