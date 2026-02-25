/*
 * Platform function registry implementation.
 *
 * Maps function names to PlatformFn pointers (concrete) or
 * GenericPlatformFactory functions (type-parameterised).
 */
#include "east/platform.h"
#include "east/hashmap.h"
#include <stdlib.h>
#include <string.h>

PlatformRegistry *platform_registry_new(void)
{
    PlatformRegistry *reg = calloc(1, sizeof(PlatformRegistry));
    if (!reg) return NULL;
    reg->functions = hashmap_new();
    reg->generic_functions = hashmap_new();
    return reg;
}

void platform_registry_add(PlatformRegistry *reg, const char *name,
                           PlatformFn fn, bool is_async)
{
    if (!reg || !name) return;
    PlatformFunction *pf = calloc(1, sizeof(PlatformFunction));
    if (!pf) return;
    pf->name = name;
    pf->fn = fn;
    pf->is_async = is_async;
    hashmap_set(reg->functions, name, pf);
}

void platform_registry_add_generic(PlatformRegistry *reg, const char *name,
                                   GenericPlatformFactory factory, bool is_async)
{
    if (!reg || !name) return;
    GenericPlatformFunction *gf = calloc(1, sizeof(GenericPlatformFunction));
    if (!gf) return;
    gf->name = name;
    gf->factory = factory;
    gf->is_async = is_async;
    hashmap_set(reg->generic_functions, name, gf);
}

PlatformFn platform_registry_get(PlatformRegistry *reg, const char *name,
                                 EastType **type_params, size_t num_tp)
{
    if (!reg || !name) return NULL;

    /* Try concrete functions first. */
    PlatformFunction *pf = hashmap_get(reg->functions, name);
    if (pf) return pf->fn;

    /* Try generic functions. */
    GenericPlatformFunction *gf = hashmap_get(reg->generic_functions, name);
    if (gf) return gf->factory(type_params, num_tp);

    return NULL;
}

static void free_pf(void *v) { free(v); }

void platform_registry_free(PlatformRegistry *reg)
{
    if (!reg) return;
    hashmap_free(reg->functions, free_pf);
    hashmap_free(reg->generic_functions, free_pf);
    free(reg);
}
