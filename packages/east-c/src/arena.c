#include "east/arena.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Thin wrappers around the C allocator                               */
/* ------------------------------------------------------------------ */

void *east_alloc(size_t size) {
    return malloc(size);
}

void *east_calloc(size_t count, size_t size) {
    return calloc(count, size);
}

void *east_realloc(void *ptr, size_t old_size, size_t new_size) {
    (void)old_size;
    return realloc(ptr, new_size);
}

char *east_strdup(const char *s) {
    if (!s) return NULL;
    return strdup(s);
}

char *east_strndup(const char *s, size_t n) {
    if (!s) return NULL;
    size_t len = strlen(s);
    if (len > n) len = n;
    char *p = malloc(len + 1);
    if (p) { memcpy(p, s, len); p[len] = '\0'; }
    return p;
}

void east_free(void *ptr) {
    free(ptr);
}
