#ifndef EAST_GC_H
#define EAST_GC_H

#include <stddef.h>

typedef struct EastValue EastValue;

/* Add a value to the GC tracking list.  Called automatically by
 * alloc_value() for cycle-capable types (array, set, dict, struct,
 * variant, ref, function). */
void east_gc_track(EastValue *v);

/* Remove a value from the GC tracking list. */
void east_gc_untrack(EastValue *v);

/* Run the cycle collector.  Finds and frees reference cycles among
 * tracked objects using CPython-style trial deletion. */
void east_gc_collect(void);

/* Number of objects currently tracked. */
size_t east_gc_tracked_count(void);

#endif
