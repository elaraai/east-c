#ifndef EAST_ARENA_H
#define EAST_ARENA_H

#include <stddef.h>

/*
 * Allocation wrappers.
 *
 * Previously these routed through an arena allocator; now they are thin
 * wrappers around the standard C allocator.  The interface is kept so that
 * call sites throughout the codebase do not need to change.
 */

void *east_alloc(size_t size);
void *east_calloc(size_t count, size_t size);
void *east_realloc(void *ptr, size_t old_size, size_t new_size);
char *east_strdup(const char *s);
char *east_strndup(const char *s, size_t n);
void  east_free(void *ptr);

#endif
