#include "east/hashmap.h"
#include "east/arena.h"

#include <stdlib.h>
#include <string.h>

#define HASHMAP_INITIAL_CAPACITY 16
#define HASHMAP_LOAD_FACTOR 0.75

/*
 * Sentinel key used to mark tombstone entries. Points to a private static
 * variable so it can never collide with a real heap-allocated key.
 */
static char TOMBSTONE_SENTINEL;
#define TOMBSTONE_KEY (&TOMBSTONE_SENTINEL)

/* FNV-1a hash for null-terminated strings. */
static size_t fnv1a(const char *key) {
    size_t hash = 14695981039346656037ULL;
    for (const char *p = key; *p; p++) {
        hash ^= (unsigned char)*p;
        hash *= 1099511628211ULL;
    }
    return hash;
}

/* Find the index for a key. Returns the index of either the matching occupied
 * slot or the first available slot (empty or tombstone) where the key could be
 * inserted. When `for_insert` is true the search stops at tombstones so they
 * can be reused; when false it skips tombstones to find a true match. */
static size_t find_index(HashmapEntry *entries, size_t capacity,
                         const char *key, bool for_insert) {
    size_t idx = fnv1a(key) & (capacity - 1);
    size_t tombstone_idx = capacity; /* capacity = "none found" */

    for (;;) {
        HashmapEntry *e = &entries[idx];

        if (!e->occupied && e->key != TOMBSTONE_KEY) {
            /* Empty slot -- key is not in the table. */
            return (for_insert && tombstone_idx != capacity)
                       ? tombstone_idx
                       : idx;
        }

        if (e->key == TOMBSTONE_KEY) {
            /* Tombstone -- remember it for possible reuse on insert. */
            if (for_insert && tombstone_idx == capacity) {
                tombstone_idx = idx;
            }
        } else if (e->occupied && strcmp(e->key, key) == 0) {
            /* Found a live entry with a matching key. */
            return idx;
        }

        idx = (idx + 1) & (capacity - 1);
    }
}

/* Resize the table to `new_capacity` and re-insert all live entries. */
static void hashmap_resize(Hashmap *map, size_t new_capacity) {
    HashmapEntry *old_entries = map->entries;
    size_t old_capacity = map->capacity;

    map->entries = east_calloc(new_capacity, sizeof(HashmapEntry));
    map->capacity = new_capacity;
    map->count = 0;

    for (size_t i = 0; i < old_capacity; i++) {
        HashmapEntry *e = &old_entries[i];
        if (e->occupied && e->key != TOMBSTONE_KEY) {
            size_t idx = find_index(map->entries, new_capacity, e->key, true);
            map->entries[idx].key = e->key;      /* transfer ownership */
            map->entries[idx].value = e->value;
            map->entries[idx].occupied = true;
            map->count++;
        }
    }

    east_free(old_entries);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

Hashmap *hashmap_new(void) {
    Hashmap *map = east_alloc(sizeof(Hashmap));
    if (!map) return NULL;

    map->entries = east_calloc(HASHMAP_INITIAL_CAPACITY, sizeof(HashmapEntry));
    if (!map->entries) {
        east_free(map);
        return NULL;
    }

    map->capacity = HASHMAP_INITIAL_CAPACITY;
    map->count = 0;
    return map;
}

void hashmap_free(Hashmap *map, void (*free_value)(void *)) {
    if (!map) return;

    for (size_t i = 0; i < map->capacity; i++) {
        HashmapEntry *e = &map->entries[i];
        if (e->occupied && e->key != TOMBSTONE_KEY) {
            east_free(e->key);
            if (free_value && e->value) {
                free_value(e->value);
            }
        }
    }

    east_free(map->entries);
    east_free(map);
}

void *hashmap_get(Hashmap *map, const char *key) {
    if (!map || !key) return NULL;

    size_t idx = find_index(map->entries, map->capacity, key, false);
    HashmapEntry *e = &map->entries[idx];

    if (e->occupied && e->key != TOMBSTONE_KEY) {
        return e->value;
    }
    return NULL;
}

void hashmap_set(Hashmap *map, const char *key, void *value) {
    if (!map || !key) return;

    /* Grow if load factor would be exceeded. */
    if ((map->count + 1) > (size_t)(map->capacity * HASHMAP_LOAD_FACTOR)) {
        hashmap_resize(map, map->capacity * 2);
    }

    size_t idx = find_index(map->entries, map->capacity, key, true);
    HashmapEntry *e = &map->entries[idx];

    if (e->occupied && e->key != TOMBSTONE_KEY) {
        /* Key already exists -- update value in place. */
        e->value = value;
        return;
    }

    /* New insertion (into empty slot or tombstone). */
    e->key = east_strdup(key);
    e->value = value;
    e->occupied = true;
    map->count++;
}

bool hashmap_has(Hashmap *map, const char *key) {
    if (!map || !key) return false;

    size_t idx = find_index(map->entries, map->capacity, key, false);
    HashmapEntry *e = &map->entries[idx];

    return e->occupied && e->key != TOMBSTONE_KEY;
}

void hashmap_delete(Hashmap *map, const char *key,
                    void (*free_value)(void *)) {
    if (!map || !key) return;

    size_t idx = find_index(map->entries, map->capacity, key, false);
    HashmapEntry *e = &map->entries[idx];

    if (!e->occupied || e->key == TOMBSTONE_KEY) {
        return; /* Key not found. */
    }

    east_free(e->key);
    if (free_value && e->value) {
        free_value(e->value);
    }

    /* Mark as tombstone: occupied stays true so linear probing continues
     * past this slot, but key is set to the sentinel. */
    e->key = TOMBSTONE_KEY;
    e->value = NULL;
    /* e->occupied remains true */
    map->count--;
}

size_t hashmap_count(Hashmap *map) {
    if (!map) return 0;
    return map->count;
}

void hashmap_iter(Hashmap *map, HashmapIterFn fn, void *ctx) {
    if (!map || !fn) return;

    for (size_t i = 0; i < map->capacity; i++) {
        HashmapEntry *e = &map->entries[i];
        if (e->occupied && e->key != TOMBSTONE_KEY) {
            fn(e->key, e->value, ctx);
        }
    }
}

char **hashmap_keys(Hashmap *map, size_t *out_count) {
    if (!map) {
        if (out_count) *out_count = 0;
        return NULL;
    }

    char **keys = malloc(map->count * sizeof(char *));
    if (!keys) {
        if (out_count) *out_count = 0;
        return NULL;
    }

    size_t n = 0;
    for (size_t i = 0; i < map->capacity; i++) {
        HashmapEntry *e = &map->entries[i];
        if (e->occupied && e->key != TOMBSTONE_KEY) {
            keys[n++] = e->key;
        }
    }

    if (out_count) *out_count = n;
    return keys;
}
