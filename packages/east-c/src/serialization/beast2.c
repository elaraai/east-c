/*
 * BEAST2 binary serialization for East types.
 *
 * Headerless binary format with varint encoding.
 * Type-driven: no type tags in the output; the type guides encoding/decoding.
 *
 * Encoding format:
 *   Null:     nothing (0 bytes)
 *   Boolean:  1 byte (0 or 1)
 *   Integer:  zigzag-encoded varint
 *   Float:    8 bytes little-endian IEEE 754
 *   String:   varint length + UTF-8 bytes
 *   DateTime: zigzag varint (epoch millis)
 *   Blob:     varint length + raw bytes
 *   Array:    varint count + each element
 *   Set:      varint count + each element
 *   Dict:     varint count + each key-value pair
 *   Struct:   each field in schema order
 *   Variant:  varint case index + case value
 *   Ref:      encode inner value
 *   Vector:   varint length + packed elements
 *   Matrix:   varint rows + varint cols + packed elements
 */

#include "east/serialization.h"
#include "east/types.h"
#include "east/values.h"
#include "east/compiler.h"
#include "east/type_of_type.h"
#include "east/env.h"
#include "east/ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef BEAST2_PROFILE_DEDUP
#include <time.h>
#endif

/* ================================================================== */
/*  Helpers for little-endian float writing/reading                    */
/* ================================================================== */

static void write_float64_le(ByteBuffer *buf, double val)
{
    uint8_t bytes[8];
    memcpy(bytes, &val, 8);
    /* On big-endian systems this would need byte-swapping.
     * Assuming little-endian (x86, ARM LE) for simplicity. */
    byte_buffer_write_bytes(buf, bytes, 8);
}

static double read_float64_le(const uint8_t *data, size_t *offset)
{
    double val;
    memcpy(&val, data + *offset, 8);
    *offset += 8;
    return val;
}

/* Read a varint-prefixed string, returning malloc'd string and setting *out_len */
static char *read_string_varint(const uint8_t *data, size_t len, size_t *offset, size_t *out_len)
{
    uint64_t slen = read_varint(data, offset);
    if (*offset + slen > len) { *out_len = 0; return NULL; }
    char *str = malloc(slen + 1);
    if (!str) { *out_len = 0; return NULL; }
    memcpy(str, data + *offset, slen);
    str[slen] = '\0';
    *offset += slen;
    *out_len = (size_t)slen;
    return str;
}

/* ================================================================== */
/*  BEAST2 Backreference Context                                       */
/*                                                                     */
/*  Mutable containers (Array, Set, Dict, Ref) use a backreference     */
/*  protocol: varint(0) = inline (first occurrence), varint(N>0) =     */
/*  backreference (N = distance in bytes from current pos to stored).  */
/* ================================================================== */

/*
 * Open-addressing hash tables for O(1) backreference lookup.
 * Encode ctx: key = EastValue* pointer -> value = byte offset
 * Decode ctx: key = byte offset -> value = EastValue*
 */

typedef struct {
    uintptr_t key;     /* 0 = empty slot */
    size_t offset;
} Beast2EncSlot;

/* Global type table for IR encoding: deduplicates EastTypeValue objects
 * across all functions in a beast2 file. */
typedef struct {
    EastValue **types;      /* unique type values (retained) */
    size_t count;
    size_t capacity;
} Beast2TypeTable;

static void type_table_init(Beast2TypeTable *t) {
    t->types = NULL;
    t->count = 0;
    t->capacity = 0;
}

static void type_table_free(Beast2TypeTable *t) {
    for (size_t i = 0; i < t->count; i++)
        east_value_release(t->types[i]);
    free(t->types);
}

/* Returns the index of the type, adding it if not present (structural equality). */
static size_t type_table_add(Beast2TypeTable *t, EastValue *type_val) {
    for (size_t i = 0; i < t->count; i++) {
        if (east_value_equal(t->types[i], type_val)) return i;
    }
    if (t->count >= t->capacity) {
        t->capacity = t->capacity ? t->capacity * 2 : 16;
        t->types = realloc(t->types, t->capacity * sizeof(EastValue*));
    }
    east_value_retain(type_val);
    t->types[t->count] = type_val;
    return t->count++;
}

static int type_table_find(Beast2TypeTable *t, EastValue *type_val) {
    for (size_t i = 0; i < t->count; i++) {
        if (east_value_equal(t->types[i], type_val)) return (int)i;
    }
    return -1;
}

typedef struct {
    Beast2EncSlot *slots;
    int mask;          /* capacity - 1 (capacity is power of 2) */
    int count;
    /* Optional: when set, function values are encoded as handle IDs */
    Beast2HandleAllocFn fn_handle_alloc;
    void *fn_handle_user_data;
    /* Global type table for IR encoding (NULL if not used) */
    Beast2TypeTable *global_type_table;
} Beast2EncodeCtx;

typedef struct {
    size_t key;        /* 0 = empty (offset 0 never used as backreference target) */
    EastValue *value;
} Beast2DecSlot;

/* Value dedup: identical byte ranges (under the same type) produce the same
 * EastValue pointer.  This enables O(1) pointer-equality caching downstream
 * (e.g. TypeCache in type_of_type.c). */
typedef struct {
    uint64_t hash;       /* 0 = empty slot */
    size_t byte_start;
    size_t byte_len;
    EastType *type;
    EastValue *value;
} Beast2DedupSlot;

typedef struct {
    Beast2DecSlot *slots;
    int mask;
    int count;
    /* Struct/Variant value dedup */
    Beast2DedupSlot *dedup_slots;
    int dedup_mask;
    int dedup_count;
    /* Backreference tracking: incremented when a backref is resolved.
     * Struct/Variant dedup is unsafe when backrefs were used because
     * backreference distances are relative to buffer position, so
     * identical bytes at different positions resolve to different targets. */
    int backref_count;
    /* Profiling counters (always present, negligible cost) */
    int dedup_hits;
    int dedup_misses;
    size_t dedup_bytes_hashed;
    /* Global type table for IR decoding (NULL if not used) */
    EastValue **global_type_table;
    size_t global_type_table_size;
#ifdef BEAST2_PROFILE_DEDUP
    /* Per-type dedup stats: open-addressing table keyed by EastType* */
    struct {
        EastType *type;
        int hits;
        int misses;
        size_t bytes_hashed;
        double time_us;  /* cumulative microseconds spent hashing+lookup+insert */
    } *type_stats;
    int type_stats_mask;
    int type_stats_count;
#endif
} Beast2DecodeCtx;

static inline uint32_t hash_ptr(uintptr_t p)
{
    /* Fibonacci hashing — good distribution for pointer values */
    p ^= p >> 16;
    p *= 0x45d9f3b;
    p ^= p >> 16;
    return (uint32_t)p;
}

static inline uint32_t hash_offset(size_t o)
{
    uintptr_t p = (uintptr_t)o;
    p ^= p >> 16;
    p *= 0x45d9f3b;
    p ^= p >> 16;
    return (uint32_t)p;
}

static void beast2_enc_ctx_init(Beast2EncodeCtx *ctx)
{
    ctx->mask = 63;  /* initial capacity 64 */
    ctx->count = 0;
    ctx->slots = calloc((size_t)(ctx->mask + 1), sizeof(Beast2EncSlot));
    ctx->fn_handle_alloc = NULL;
    ctx->fn_handle_user_data = NULL;
    ctx->global_type_table = NULL;
}

static void beast2_enc_ctx_free(Beast2EncodeCtx *ctx)
{
    free(ctx->slots);
}

static void beast2_enc_ctx_grow(Beast2EncodeCtx *ctx)
{
    int old_cap = ctx->mask + 1;
    int new_cap = old_cap * 2;
    int new_mask = new_cap - 1;
    Beast2EncSlot *new_slots = calloc((size_t)new_cap, sizeof(Beast2EncSlot));
    if (!new_slots) return;

    for (int i = 0; i < old_cap; i++) {
        if (ctx->slots[i].key != 0) {
            uint32_t h = hash_ptr(ctx->slots[i].key) & (uint32_t)new_mask;
            while (new_slots[h].key != 0)
                h = (h + 1) & (uint32_t)new_mask;
            new_slots[h] = ctx->slots[i];
        }
    }
    free(ctx->slots);
    ctx->slots = new_slots;
    ctx->mask = new_mask;
}

/* Look up a value in the encode context. Returns -1 if not found, else the stored offset. */
static int beast2_enc_ctx_find(Beast2EncodeCtx *ctx, EastValue *value)
{
    uintptr_t key = (uintptr_t)value;
    uint32_t h = hash_ptr(key) & (uint32_t)ctx->mask;
    for (;;) {
        if (ctx->slots[h].key == key)
            return (int)ctx->slots[h].offset;
        if (ctx->slots[h].key == 0)
            return -1;
        h = (h + 1) & (uint32_t)ctx->mask;
    }
}

static void beast2_enc_ctx_add(Beast2EncodeCtx *ctx, EastValue *value, size_t offset)
{
    /* Grow at 70% load */
    if (ctx->count * 10 >= (ctx->mask + 1) * 7)
        beast2_enc_ctx_grow(ctx);

    uintptr_t key = (uintptr_t)value;
    uint32_t h = hash_ptr(key) & (uint32_t)ctx->mask;
    while (ctx->slots[h].key != 0)
        h = (h + 1) & (uint32_t)ctx->mask;
    ctx->slots[h].key = key;
    ctx->slots[h].offset = offset;
    ctx->count++;
}

static void beast2_dec_ctx_init(Beast2DecodeCtx *ctx)
{
    ctx->mask = 63;
    ctx->count = 0;
    ctx->slots = calloc((size_t)(ctx->mask + 1), sizeof(Beast2DecSlot));
    ctx->dedup_mask = 4095;  /* initial capacity 4096 */
    ctx->dedup_count = 0;
    ctx->dedup_slots = calloc((size_t)(ctx->dedup_mask + 1), sizeof(Beast2DedupSlot));
    ctx->backref_count = 0;
    ctx->dedup_hits = 0;
    ctx->dedup_misses = 0;
    ctx->dedup_bytes_hashed = 0;
    ctx->global_type_table = NULL;
    ctx->global_type_table_size = 0;
#ifdef BEAST2_PROFILE_DEDUP
    ctx->type_stats_mask = 255;  /* 256 slots */
    ctx->type_stats_count = 0;
    ctx->type_stats = calloc(256, sizeof(ctx->type_stats[0]));
#endif
}

static void beast2_dec_ctx_free(Beast2DecodeCtx *ctx)
{
    /* Release all backref'd values */
    if (ctx->slots) {
        for (int i = 0; i <= ctx->mask; i++) {
            if (ctx->slots[i].key != 0 && ctx->slots[i].value) {
                east_value_release(ctx->slots[i].value);
            }
        }
    }
    free(ctx->slots);
    /* Release all dedup'd values */
    if (ctx->dedup_slots) {
        for (int i = 0; i <= ctx->dedup_mask; i++) {
            if (ctx->dedup_slots[i].hash != 0 && ctx->dedup_slots[i].value) {
                east_value_release(ctx->dedup_slots[i].value);
            }
        }
    }
    free(ctx->dedup_slots);
#ifdef BEAST2_PROFILE_DEDUP
    free(ctx->type_stats);
#endif
}

static void beast2_dec_ctx_grow(Beast2DecodeCtx *ctx)
{
    int old_cap = ctx->mask + 1;
    int new_cap = old_cap * 2;
    int new_mask = new_cap - 1;
    Beast2DecSlot *new_slots = calloc((size_t)new_cap, sizeof(Beast2DecSlot));
    if (!new_slots) return;

    for (int i = 0; i < old_cap; i++) {
        if (ctx->slots[i].key != 0) {
            uint32_t h = hash_offset(ctx->slots[i].key) & (uint32_t)new_mask;
            while (new_slots[h].key != 0)
                h = (h + 1) & (uint32_t)new_mask;
            new_slots[h] = ctx->slots[i];
        }
    }
    free(ctx->slots);
    ctx->slots = new_slots;
    ctx->mask = new_mask;
}

/* Look up by offset in the decode context. Returns NULL if not found. */
static EastValue *beast2_dec_ctx_find(Beast2DecodeCtx *ctx, size_t offset)
{
    if (offset == 0) return NULL;
    uint32_t h = hash_offset(offset) & (uint32_t)ctx->mask;
    for (;;) {
        if (ctx->slots[h].key == offset)
            return ctx->slots[h].value;
        if (ctx->slots[h].key == 0)
            return NULL;
        h = (h + 1) & (uint32_t)ctx->mask;
    }
}

static void beast2_dec_ctx_add(Beast2DecodeCtx *ctx, EastValue *value, size_t offset)
{
    if (offset == 0) return;  /* offset 0 is reserved as empty sentinel */
    if (ctx->count * 10 >= (ctx->mask + 1) * 7)
        beast2_dec_ctx_grow(ctx);

    uint32_t h = hash_offset(offset) & (uint32_t)ctx->mask;
    while (ctx->slots[h].key != 0)
        h = (h + 1) & (uint32_t)ctx->mask;
    ctx->slots[h].key = offset;
    east_value_retain(value);  /* backref table owns a reference */
    ctx->slots[h].value = value;
    ctx->count++;
}

/* ================================================================== */
/*  BEAST2 Value Dedup (byte-range based)                              */
/* ================================================================== */

/*
 * Full-content hash using wyhash-style mixing.
 *
 * Hashes ALL bytes (not just head/tail), so collisions are extremely rare
 * (~2^-64). This eliminates nearly all memcmp calls in dedup_find at the
 * cost of reading all bytes during hashing — a net win because the memcmp
 * cascade from collisions was far more expensive.
 */
static inline uint64_t wymix(uint64_t a, uint64_t b)
{
    __uint128_t r = (__uint128_t)a * b;
    return (uint64_t)(r >> 64) ^ (uint64_t)r;
}

static inline uint64_t wyread8(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint64_t wyread4(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static uint64_t hash_byte_range(const uint8_t *data, size_t len, uintptr_t type_ptr)
{
    const uint64_t s0 = 0xa0761d6478bd642fULL;
    const uint64_t s1 = 0xe7037ed1a0b428dbULL;
    const uint64_t s2 = 0x8ebc6af09c88c6e3ULL;
    const uint64_t s3 = 0x589965cc75374cc3ULL;

    uint64_t seed = s0 ^ type_ptr;
    const uint8_t *p = data;
    uint64_t a, b;

    if (len <= 16) {
        if (len >= 4) {
            a = (wyread4(p) << 32) | wyread4(p + ((len >> 3) << 2));
            b = (wyread4(p + len - 4) << 32) | wyread4(p + len - 4 - ((len >> 3) << 2));
        } else if (len > 0) {
            a = ((uint64_t)p[0] << 16) | ((uint64_t)p[len >> 1] << 8) | p[len - 1];
            b = 0;
        } else {
            a = b = 0;
        }
    } else if (len <= 48) {
        a = wymix(wyread8(p) ^ s1, wyread8(p + 8) ^ seed);
        b = wymix(wyread8(p + len - 16) ^ s2, wyread8(p + len - 8) ^ seed);
        if (len > 32) {
            a ^= wymix(wyread8(p + 16) ^ s3, wyread8(p + 24) ^ seed);
        }
    } else {
        /* Process 48-byte chunks */
        uint64_t see1 = seed, see2 = seed;
        size_t i = len;
        while (i > 48) {
            seed = wymix(wyread8(p) ^ s1, wyread8(p + 8) ^ seed);
            see1 = wymix(wyread8(p + 16) ^ s2, wyread8(p + 24) ^ see1);
            see2 = wymix(wyread8(p + 32) ^ s3, wyread8(p + 40) ^ see2);
            p += 48;
            i -= 48;
        }
        seed ^= see1 ^ see2;
        /* Process remaining bytes */
        a = wymix(wyread8(p + i - 16) ^ s1, wyread8(p + i - 8) ^ seed);
        b = wymix(wyread8(p + i - 48) ^ s2, wyread8(p + i - 40) ^ seed);
    }

    uint64_t h = wymix(s1 ^ len, wymix(a ^ s1, b ^ seed));
    return h ? h : 1;
}

static void beast2_dedup_grow(Beast2DecodeCtx *ctx)
{
    int old_cap = ctx->dedup_mask + 1;
    int new_cap = old_cap * 2;
    int new_mask = new_cap - 1;
    Beast2DedupSlot *new_slots = calloc((size_t)new_cap, sizeof(Beast2DedupSlot));
    if (!new_slots) return;

    for (int i = 0; i < old_cap; i++) {
        if (ctx->dedup_slots[i].hash != 0) {
            uint32_t h = (uint32_t)(ctx->dedup_slots[i].hash) & (uint32_t)new_mask;
            while (new_slots[h].hash != 0)
                h = (h + 1) & (uint32_t)new_mask;
            new_slots[h] = ctx->dedup_slots[i];
        }
    }
    free(ctx->dedup_slots);
    ctx->dedup_slots = new_slots;
    ctx->dedup_mask = new_mask;
}

static EastValue *beast2_dedup_find(Beast2DecodeCtx *ctx, uint64_t hash,
                                     const uint8_t *data, size_t byte_start,
                                     size_t byte_len, EastType *type)
{
    (void)data; (void)byte_start; /* no longer needed — full-content hash is sufficient */
    uint32_t h = (uint32_t)(hash) & (uint32_t)ctx->dedup_mask;
    for (;;) {
        if (ctx->dedup_slots[h].hash == 0) return NULL;
        if (ctx->dedup_slots[h].hash == hash &&
            ctx->dedup_slots[h].byte_len == byte_len &&
            ctx->dedup_slots[h].type == type) {
            return ctx->dedup_slots[h].value;
        }
        h = (h + 1) & (uint32_t)ctx->dedup_mask;
    }
}

static void beast2_dedup_add(Beast2DecodeCtx *ctx, uint64_t hash,
                              size_t byte_start, size_t byte_len,
                              EastType *type, EastValue *value)
{
    if (ctx->dedup_count * 10 >= (ctx->dedup_mask + 1) * 7)
        beast2_dedup_grow(ctx);

    uint32_t h = (uint32_t)(hash) & (uint32_t)ctx->dedup_mask;
    while (ctx->dedup_slots[h].hash != 0)
        h = (h + 1) & (uint32_t)ctx->dedup_mask;
    ctx->dedup_slots[h].hash = hash;
    ctx->dedup_slots[h].byte_start = byte_start;
    ctx->dedup_slots[h].byte_len = byte_len;
    ctx->dedup_slots[h].type = type;
    east_value_retain(value);  /* dedup table owns a reference */
    ctx->dedup_slots[h].value = value;
    ctx->dedup_count++;
}

#ifdef BEAST2_PROFILE_DEDUP
static inline double beast2_clock_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* Find or create a per-type stats entry. Returns pointer to the stats slot. */
static inline typeof(((Beast2DecodeCtx*)0)->type_stats[0]) *
beast2_type_stats_get(Beast2DecodeCtx *ctx, EastType *type)
{
    uint32_t h = (uint32_t)((uintptr_t)type * 0x45d9f3bU) & (uint32_t)ctx->type_stats_mask;
    for (;;) {
        if (ctx->type_stats[h].type == type)
            return &ctx->type_stats[h];
        if (ctx->type_stats[h].type == NULL) {
            /* New entry */
            ctx->type_stats[h].type = type;
            ctx->type_stats_count++;
            /* Grow at 70% load */
            if (ctx->type_stats_count * 10 >= (ctx->type_stats_mask + 1) * 7) {
                int old_cap = ctx->type_stats_mask + 1;
                int new_cap = old_cap * 2;
                int new_mask = new_cap - 1;
                typeof(ctx->type_stats) new_table = calloc(new_cap, sizeof(ctx->type_stats[0]));
                for (int i = 0; i < old_cap; i++) {
                    if (ctx->type_stats[i].type) {
                        uint32_t nh = (uint32_t)((uintptr_t)ctx->type_stats[i].type * 0x45d9f3bU) & (uint32_t)new_mask;
                        while (new_table[nh].type) nh = (nh + 1) & (uint32_t)new_mask;
                        new_table[nh] = ctx->type_stats[i];
                    }
                }
                free(ctx->type_stats);
                ctx->type_stats = new_table;
                ctx->type_stats_mask = new_mask;
                /* Re-lookup after grow */
                h = (uint32_t)((uintptr_t)type * 0x45d9f3bU) & (uint32_t)ctx->type_stats_mask;
                while (ctx->type_stats[h].type != type)
                    h = (h + 1) & (uint32_t)ctx->type_stats_mask;
            }
            return &ctx->type_stats[h];
        }
        h = (h + 1) & (uint32_t)ctx->type_stats_mask;
    }
}

static void beast2_dedup_print_stats(Beast2DecodeCtx *ctx)
{
    fprintf(stderr, "\n=== Beast2 Dedup Stats ===\n");
    fprintf(stderr, "Total: hits=%d misses=%d bytes_hashed=%zu\n",
            ctx->dedup_hits, ctx->dedup_misses, ctx->dedup_bytes_hashed);

    /* Collect and sort per-type stats by time descending */
    int n = 0;
    for (int i = 0; i <= ctx->type_stats_mask; i++) {
        if (ctx->type_stats[i].type) n++;
    }
    if (n == 0) return;

    /* Flatten into array for sorting */
    typedef struct { EastType *type; int hits; int misses; size_t bytes; double time_us; } Entry;
    Entry *entries = malloc(n * sizeof(Entry));
    int ei = 0;
    for (int i = 0; i <= ctx->type_stats_mask; i++) {
        if (ctx->type_stats[i].type) {
            entries[ei].type = ctx->type_stats[i].type;
            entries[ei].hits = ctx->type_stats[i].hits;
            entries[ei].misses = ctx->type_stats[i].misses;
            entries[ei].bytes = ctx->type_stats[i].bytes_hashed;
            entries[ei].time_us = ctx->type_stats[i].time_us;
            ei++;
        }
    }
    /* Simple insertion sort by time descending */
    for (int i = 1; i < n; i++) {
        Entry tmp = entries[i];
        int j = i - 1;
        while (j >= 0 && entries[j].time_us < tmp.time_us) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = tmp;
    }

    fprintf(stderr, "\nPer-type dedup breakdown (sorted by time):\n");
    fprintf(stderr, "%-12s %8s %8s %12s %10s  %s\n",
            "TYPE_KIND", "HITS", "MISSES", "BYTES", "TIME_MS", "TYPE_PTR");
    double total_time = 0;
    for (int i = 0; i < n; i++) total_time += entries[i].time_us;
    for (int i = 0; i < n; i++) {
        const char *kind_name = east_type_kind_name(entries[i].type->kind);
        fprintf(stderr, "%-12s %8d %8d %12zu %10.1f  %p",
                kind_name,
                entries[i].hits, entries[i].misses,
                entries[i].bytes,
                entries[i].time_us / 1000.0,
                (void*)entries[i].type);
        /* For struct/variant, print brief type info */
        if (entries[i].type->kind == EAST_TYPE_STRUCT && entries[i].type->data.struct_.num_fields > 0) {
            fprintf(stderr, "  {%s", entries[i].type->data.struct_.fields[0].name);
            if (entries[i].type->data.struct_.num_fields > 1)
                fprintf(stderr, ", %s", entries[i].type->data.struct_.fields[1].name);
            if (entries[i].type->data.struct_.num_fields > 2)
                fprintf(stderr, ", ...[%zu fields]", entries[i].type->data.struct_.num_fields);
            fprintf(stderr, "}");
        } else if (entries[i].type->kind == EAST_TYPE_VARIANT && entries[i].type->data.variant.num_cases > 0) {
            fprintf(stderr, "  |%s", entries[i].type->data.variant.cases[0].name);
            if (entries[i].type->data.variant.num_cases > 1)
                fprintf(stderr, "|%s", entries[i].type->data.variant.cases[1].name);
            if (entries[i].type->data.variant.num_cases > 2)
                fprintf(stderr, "|...[%zu cases]", entries[i].type->data.variant.num_cases);
        }
        fprintf(stderr, "  (%.1f%%)\n", entries[i].time_us * 100.0 / total_time);
    }
    fprintf(stderr, "Total dedup time: %.1f ms\n", total_time / 1000.0);
    free(entries);
}
#endif

/* ================================================================== */
/*  Generic type-directed IR value walkers                             */
/*  Walk an EastValue tree guided by an EastType, finding/transforming */
/*  values at positions where the type matches a target type.          */
/* ================================================================== */

typedef void (*TypeVisitFn)(EastValue *val, void *ctx);
typedef EastValue *(*TypeTransformFn)(EastValue *val, void *ctx);

/* Visit all positions in a value tree where the guide type == target. */
static void visit_type_positions(EastValue *value, EastType *type,
                                  EastType *target, TypeVisitFn cb, void *ctx)
{
    if (!value) return;
    if (type == target) { cb(value, ctx); return; }

    switch (type->kind) {
    case EAST_TYPE_RECURSIVE:
        if (type->data.recursive.node)
            visit_type_positions(value, type->data.recursive.node, target, cb, ctx);
        break;
    case EAST_TYPE_STRUCT:
        if (value->kind == EAST_VAL_STRUCT) {
            for (size_t i = 0; i < type->data.struct_.num_fields && i < value->data.struct_.num_fields; i++)
                visit_type_positions(value->data.struct_.field_values[i],
                                     type->data.struct_.fields[i].type, target, cb, ctx);
        }
        break;
    case EAST_TYPE_VARIANT:
        if (value->kind == EAST_VAL_VARIANT) {
            const char *cn = value->data.variant.case_name;
            for (size_t i = 0; i < type->data.variant.num_cases; i++) {
                if (strcmp(type->data.variant.cases[i].name, cn) == 0) {
                    visit_type_positions(value->data.variant.value,
                                         type->data.variant.cases[i].type, target, cb, ctx);
                    break;
                }
            }
        }
        break;
    case EAST_TYPE_ARRAY:
        if (value->kind == EAST_VAL_ARRAY) {
            for (size_t i = 0; i < value->data.array.len; i++)
                visit_type_positions(value->data.array.items[i],
                                     type->data.element, target, cb, ctx);
        }
        break;
    default:
        break;
    }
}

/* Transform all positions in a value tree where the guide type == target.
 * Returns a new retained value (may share structure if no changes). */
static EastValue *transform_type_positions(EastValue *value, EastType *type,
                                            EastType *target,
                                            TypeTransformFn transform, void *ctx)
{
    if (!value) return NULL;
    if (type == target) return transform(value, ctx);

    switch (type->kind) {
    case EAST_TYPE_RECURSIVE:
        if (type->data.recursive.node)
            return transform_type_positions(value, type->data.recursive.node,
                                             target, transform, ctx);
        east_value_retain(value);
        return value;
    case EAST_TYPE_STRUCT: {
        if (value->kind != EAST_VAL_STRUCT) { east_value_retain(value); return value; }
        size_t nf = type->data.struct_.num_fields;
        if (nf > value->data.struct_.num_fields) nf = value->data.struct_.num_fields;
        const char **names = malloc(nf * sizeof(char*));
        EastValue **values = malloc(nf * sizeof(EastValue*));
        bool changed = false;
        for (size_t i = 0; i < nf; i++) {
            names[i] = value->data.struct_.field_names[i];
            EastValue *fval = value->data.struct_.field_values[i];
            values[i] = transform_type_positions(fval, type->data.struct_.fields[i].type,
                                                  target, transform, ctx);
            if (values[i] != fval) changed = true;
        }
        EastValue *result;
        if (changed) {
            result = east_struct_new(names, values, nf, value->data.struct_.type);
        } else {
            east_value_retain(value);
            result = value;
        }
        for (size_t i = 0; i < nf; i++) east_value_release(values[i]);
        free(names);
        free(values);
        return result;
    }
    case EAST_TYPE_VARIANT: {
        if (value->kind != EAST_VAL_VARIANT) { east_value_retain(value); return value; }
        const char *cn = value->data.variant.case_name;
        for (size_t i = 0; i < type->data.variant.num_cases; i++) {
            if (strcmp(type->data.variant.cases[i].name, cn) == 0) {
                EastValue *new_val = transform_type_positions(
                    value->data.variant.value, type->data.variant.cases[i].type,
                    target, transform, ctx);
                if (new_val == value->data.variant.value) {
                    east_value_release(new_val);
                    east_value_retain(value);
                    return value;
                }
                EastValue *result = east_variant_new(cn, new_val, value->data.variant.type);
                east_value_release(new_val);
                return result;
            }
        }
        east_value_retain(value);
        return value;
    }
    case EAST_TYPE_ARRAY: {
        if (value->kind != EAST_VAL_ARRAY) { east_value_retain(value); return value; }
        size_t n = value->data.array.len;
        EastValue *new_arr = east_array_new(value->data.array.elem_type);
        for (size_t i = 0; i < n; i++) {
            EastValue *elem = transform_type_positions(
                value->data.array.items[i], type->data.element,
                target, transform, ctx);
            east_array_push(new_arr, elem);
            east_value_release(elem);
        }
        return new_arr;
    }
    default:
        east_value_retain(value);
        return value;
    }
}

/* Collect type callback: adds to Beast2TypeTable */
static void collect_type_cb(EastValue *val, void *ctx) {
    type_table_add((Beast2TypeTable*)ctx, val);
}

/* Substitute type → integer index */
static EastValue *substitute_type_cb(EastValue *val, void *ctx) {
    Beast2TypeTable *table = ctx;
    int idx = type_table_find(table, val);
    return east_integer(idx >= 0 ? (int64_t)idx : 0);
}

/* Restore integer index → type value */
typedef struct { EastValue **types; size_t count; } RestoreCtx;
static EastValue *restore_type_cb(EastValue *val, void *ctx) {
    RestoreCtx *rctx = ctx;
    if (val->kind == EAST_VAL_INTEGER) {
        int64_t idx = val->data.integer;
        if (idx >= 0 && (size_t)idx < rctx->count) {
            east_value_retain(rctx->types[idx]);
            return rctx->types[idx];
        }
    }
    east_value_retain(val);
    return val;
}

/* Pre-scan a typed value tree to collect all IR types from functions. */
static void pre_scan_for_functions(EastValue *value, EastType *type,
                                    Beast2TypeTable *table)
{
    if (!value) return;
    switch (type->kind) {
    case EAST_TYPE_FUNCTION:
    case EAST_TYPE_ASYNC_FUNCTION: {
        if (value->kind != EAST_VAL_FUNCTION) break;
        EastCompiledFn *fn = value->data.function.compiled;
        if (!fn || !fn->source_ir) break;
        if (!east_ir_type) east_type_of_type_init();
        /* Collect types from this function's IR */
        visit_type_positions(fn->source_ir, east_ir_type, east_type_type,
                             collect_type_cb, table);
        /* Scan capture values for nested functions */
        EastValue *fn_struct = fn->source_ir->data.variant.value;
        EastValue *caps_arr = east_struct_get_field_idx(fn_struct, 2);
        if (caps_arr && caps_arr->kind == EAST_VAL_ARRAY) {
            for (size_t i = 0; i < caps_arr->data.array.len; i++) {
                EastValue *cap_var = caps_arr->data.array.items[i];
                EastValue *cap_s = cap_var->data.variant.value;
                EastValue *type_v = east_struct_get_field_idx(cap_s, 0);
                EastValue *name_v = east_struct_get_field_idx(cap_s, 2);
                if (!type_v || !name_v) continue;
                EastType *cap_type = east_type_from_value(type_v);
                if (!cap_type) continue;
                EastValue *cap_val = env_get(fn->captures, name_v->data.string.data);
                if (cap_val) pre_scan_for_functions(cap_val, cap_type, table);
                east_type_release(cap_type);
            }
        }
        break;
    }
    case EAST_TYPE_RECURSIVE:
        if (type->data.recursive.node)
            pre_scan_for_functions(value, type->data.recursive.node, table);
        break;
    case EAST_TYPE_STRUCT:
        if (value->kind == EAST_VAL_STRUCT) {
            for (size_t i = 0; i < type->data.struct_.num_fields && i < value->data.struct_.num_fields; i++)
                pre_scan_for_functions(value->data.struct_.field_values[i],
                                       type->data.struct_.fields[i].type, table);
        }
        break;
    case EAST_TYPE_VARIANT:
        if (value->kind == EAST_VAL_VARIANT) {
            const char *cn = value->data.variant.case_name;
            for (size_t i = 0; i < type->data.variant.num_cases; i++) {
                if (strcmp(type->data.variant.cases[i].name, cn) == 0) {
                    pre_scan_for_functions(value->data.variant.value,
                                           type->data.variant.cases[i].type, table);
                    break;
                }
            }
        }
        break;
    case EAST_TYPE_ARRAY:
        if (value->kind == EAST_VAL_ARRAY) {
            for (size_t i = 0; i < value->data.array.len; i++)
                pre_scan_for_functions(value->data.array.items[i],
                                       type->data.element, table);
        }
        break;
    case EAST_TYPE_SET:
        if (value->kind == EAST_VAL_SET) {
            for (size_t i = 0; i < value->data.set.len; i++)
                pre_scan_for_functions(value->data.set.items[i],
                                       type->data.element, table);
        }
        break;
    case EAST_TYPE_DICT:
        if (value->kind == EAST_VAL_DICT) {
            for (size_t i = 0; i < value->data.dict.len; i++) {
                pre_scan_for_functions(value->data.dict.keys[i],
                                       type->data.dict.key, table);
                pre_scan_for_functions(value->data.dict.values[i],
                                       type->data.dict.value, table);
            }
        }
        break;
    case EAST_TYPE_REF:
        if (value->kind == EAST_VAL_REF && value->data.ref.value)
            pre_scan_for_functions(value->data.ref.value, type->data.element, table);
        break;
    default:
        break;
    }
}

/* ================================================================== */
/*  BEAST2 Encoder                                                     */
/* ================================================================== */

static void beast2_encode_value(ByteBuffer *buf, EastValue *value,
                                EastType *type, Beast2EncodeCtx *ctx);
static EastValue *beast2_decode_value(const uint8_t *data, size_t len,
                                       size_t *offset, EastType *type,
                                       Beast2DecodeCtx *ctx);

static void beast2_encode_value(ByteBuffer *buf, EastValue *value,
                                EastType *type, Beast2EncodeCtx *ctx)
{
    if (!type) return;

    switch (type->kind) {
    case EAST_TYPE_NEVER:
        break;

    case EAST_TYPE_NULL:
        break;

    case EAST_TYPE_BOOLEAN:
        byte_buffer_write_u8(buf, value->data.boolean ? 1 : 0);
        break;

    case EAST_TYPE_INTEGER:
        write_zigzag(buf, value->data.integer);
        break;

    case EAST_TYPE_FLOAT:
        write_float64_le(buf, value->data.float64);
        break;

    case EAST_TYPE_STRING: {
        size_t slen = value->data.string.len;
        write_varint(buf, (uint64_t)slen);
        byte_buffer_write_bytes(buf, (const uint8_t *)value->data.string.data, slen);
        break;
    }

    case EAST_TYPE_DATETIME:
        write_zigzag(buf, value->data.datetime);
        break;

    case EAST_TYPE_BLOB: {
        size_t blen = value->data.blob.len;
        write_varint(buf, (uint64_t)blen);
        if (blen > 0)
            byte_buffer_write_bytes(buf, value->data.blob.data, blen);
        break;
    }

    case EAST_TYPE_ARRAY: {
        /* Backreference protocol */
        int ref_offset = beast2_enc_ctx_find(ctx, value);
        if (ref_offset >= 0) {
            /* Backreference: distance from current position to stored offset */
            write_varint(buf, (uint64_t)(buf->len - (size_t)ref_offset));
            break;
        }
        /* Inline: write 0, register, then encode contents */
        write_varint(buf, 0);
        beast2_enc_ctx_add(ctx, value, buf->len);

        EastType *elem_type = type->data.element;
        size_t count = value->data.array.len;
        write_varint(buf, (uint64_t)count);
        for (size_t i = 0; i < count; i++) {
            beast2_encode_value(buf, value->data.array.items[i], elem_type, ctx);
        }
        break;
    }

    case EAST_TYPE_SET: {
        int ref_offset = beast2_enc_ctx_find(ctx, value);
        if (ref_offset >= 0) {
            write_varint(buf, (uint64_t)(buf->len - (size_t)ref_offset));
            break;
        }
        write_varint(buf, 0);
        beast2_enc_ctx_add(ctx, value, buf->len);

        EastType *elem_type = type->data.element;
        size_t count = value->data.set.len;
        write_varint(buf, (uint64_t)count);
        for (size_t i = 0; i < count; i++) {
            beast2_encode_value(buf, value->data.set.items[i], elem_type, ctx);
        }
        break;
    }

    case EAST_TYPE_DICT: {
        int ref_offset = beast2_enc_ctx_find(ctx, value);
        if (ref_offset >= 0) {
            write_varint(buf, (uint64_t)(buf->len - (size_t)ref_offset));
            break;
        }
        write_varint(buf, 0);
        beast2_enc_ctx_add(ctx, value, buf->len);

        EastType *key_type = type->data.dict.key;
        EastType *val_type = type->data.dict.value;
        size_t count = value->data.dict.len;
        write_varint(buf, (uint64_t)count);
        for (size_t i = 0; i < count; i++) {
            beast2_encode_value(buf, value->data.dict.keys[i], key_type, ctx);
            beast2_encode_value(buf, value->data.dict.values[i], val_type, ctx);
        }
        break;
    }

    case EAST_TYPE_STRUCT: {
        size_t nf = type->data.struct_.num_fields;
        /* Struct values always have fields in type schema order */
        for (size_t i = 0; i < nf; i++) {
            EastType *ftype = type->data.struct_.fields[i].type;
            EastValue *fval = (value->kind == EAST_VAL_STRUCT && i < value->data.struct_.num_fields)
                            ? value->data.struct_.field_values[i] : NULL;
            if (fval) {
                beast2_encode_value(buf, fval, ftype, ctx);
            } else {
                EastValue *null_val = east_null();
                beast2_encode_value(buf, null_val, ftype, ctx);
                east_value_release(null_val);
            }
        }
        break;
    }

    case EAST_TYPE_VARIANT: {
        const char *case_name = value->data.variant.case_name;
        for (size_t i = 0; i < type->data.variant.num_cases; i++) {
            if (strcmp(type->data.variant.cases[i].name, case_name) == 0) {
                write_varint(buf, (uint64_t)i);
                beast2_encode_value(buf, value->data.variant.value,
                                    type->data.variant.cases[i].type, ctx);
                break;
            }
        }
        break;
    }

    case EAST_TYPE_REF: {
        /* Ref also uses backreference protocol */
        int ref_offset = beast2_enc_ctx_find(ctx, value);
        if (ref_offset >= 0) {
            write_varint(buf, (uint64_t)(buf->len - (size_t)ref_offset));
            break;
        }
        write_varint(buf, 0);
        beast2_enc_ctx_add(ctx, value, buf->len);

        beast2_encode_value(buf, value->data.ref.value, type->data.element, ctx);
        break;
    }

    case EAST_TYPE_VECTOR: {
        EastType *elem_type = type->data.element;
        size_t vlen = value->data.vector.len;
        write_varint(buf, (uint64_t)vlen);

        if (elem_type->kind == EAST_TYPE_FLOAT) {
            byte_buffer_write_bytes(buf,
                (const uint8_t *)value->data.vector.data,
                vlen * sizeof(double));
        } else if (elem_type->kind == EAST_TYPE_INTEGER) {
            byte_buffer_write_bytes(buf,
                (const uint8_t *)value->data.vector.data,
                vlen * sizeof(int64_t));
        } else if (elem_type->kind == EAST_TYPE_BOOLEAN) {
            byte_buffer_write_bytes(buf,
                (const uint8_t *)value->data.vector.data,
                vlen * sizeof(bool));
        }
        break;
    }

    case EAST_TYPE_MATRIX: {
        EastType *elem_type = type->data.element;
        size_t rows = value->data.matrix.rows;
        size_t cols = value->data.matrix.cols;
        write_varint(buf, (uint64_t)rows);
        write_varint(buf, (uint64_t)cols);

        size_t count = rows * cols;
        if (elem_type->kind == EAST_TYPE_FLOAT) {
            byte_buffer_write_bytes(buf,
                (const uint8_t *)value->data.matrix.data,
                count * sizeof(double));
        } else if (elem_type->kind == EAST_TYPE_INTEGER) {
            byte_buffer_write_bytes(buf,
                (const uint8_t *)value->data.matrix.data,
                count * sizeof(int64_t));
        } else if (elem_type->kind == EAST_TYPE_BOOLEAN) {
            byte_buffer_write_bytes(buf,
                (const uint8_t *)value->data.matrix.data,
                count * sizeof(bool));
        }
        break;
    }

    case EAST_TYPE_RECURSIVE:
        if (type->data.recursive.node) {
            beast2_encode_value(buf, value, type->data.recursive.node, ctx);
        }
        break;

    case EAST_TYPE_FUNCTION:
    case EAST_TYPE_ASYNC_FUNCTION: {
        /* Handle-aware mode: write handle ID instead of IR+captures */
        if (ctx->fn_handle_alloc) {
            int handle = ctx->fn_handle_alloc(value, ctx->fn_handle_user_data);
            if (handle <= 0) break;
            write_varint(buf, (uint64_t)handle);
            break;
        }

        EastCompiledFn *fn = value->data.function.compiled;
        if (!fn || !fn->source_ir) break;

        /* Ensure IR type is initialized */
        if (!east_ir_type) east_type_of_type_init();

        /* 1. Encode the source IR variant tree (with type table substitution) */
        if (ctx->global_type_table) {
            EastValue *indexed_ir = transform_type_positions(
                fn->source_ir, east_ir_type, east_type_type,
                substitute_type_cb, ctx->global_type_table);

            {
                Beast2EncodeCtx ir_ctx;
                beast2_enc_ctx_init(&ir_ctx);
                beast2_encode_value(buf, indexed_ir, east_ir_type_with_refs, &ir_ctx);
                beast2_enc_ctx_free(&ir_ctx);
            }
            east_value_release(indexed_ir);
        } else {
            beast2_encode_value(buf, fn->source_ir, east_ir_type, ctx);
        }

        /* 2. Extract captures array from source_ir */
        EastValue *fn_struct = fn->source_ir->data.variant.value;
        EastValue *caps_arr = east_struct_get_field_idx(fn_struct, 2); /* captures */
        size_t ncaps = (caps_arr && caps_arr->kind == EAST_VAL_ARRAY) ? caps_arr->data.array.len : 0;

        /* 3. Write capture count */
        write_varint(buf, (uint64_t)ncaps);

        /* 4. For each capture, encode its value from the environment */
        for (size_t i = 0; i < ncaps; i++) {
            EastValue *cap_var = caps_arr->data.array.items[i];
            EastValue *cap_s = cap_var->data.variant.value;
            EastValue *name_v = east_struct_get_field_idx(cap_s, 2); /* name */
            EastValue *type_v = east_struct_get_field_idx(cap_s, 0); /* type */
            bool is_mutable = false;
            EastValue *mut_v = east_struct_get_field_idx(cap_s, 3); /* mutable */
            if (mut_v && mut_v->kind == EAST_VAL_BOOLEAN) is_mutable = mut_v->data.boolean;

            const char *cap_name = name_v->data.string.data;
            EastType *cap_type = east_type_from_value(type_v);

            EastValue *cap_val = env_get(fn->captures, cap_name);
            if (cap_val && is_mutable && cap_val->kind == EAST_VAL_REF) {
                EastValue *inner = east_ref_get(cap_val);
                beast2_encode_value(buf, inner, cap_type, ctx);
                east_value_release(inner);
            } else if (cap_val) {
                beast2_encode_value(buf, cap_val, cap_type, ctx);
            }

            if (cap_type) east_type_release(cap_type);
        }
        break;
    }
    }
}

ByteBuffer *east_beast2_encode(EastValue *value, EastType *type)
{
    ByteBuffer *buf = byte_buffer_new(256);
    if (!buf) return NULL;
    Beast2EncodeCtx ctx;
    beast2_enc_ctx_init(&ctx);
    beast2_encode_value(buf, value, type, &ctx);
    beast2_enc_ctx_free(&ctx);
    return buf;
}

/* ================================================================== */
/*  BEAST2 Decoder                                                     */
/* ================================================================== */

static EastValue *beast2_decode_value(const uint8_t *data, size_t len,
                                      size_t *offset, EastType *type,
                                      Beast2DecodeCtx *ctx);

static EastValue *beast2_decode_value(const uint8_t *data, size_t len,
                                      size_t *offset, EastType *type,
                                      Beast2DecodeCtx *ctx)
{
    if (!type) return NULL;

    switch (type->kind) {
    case EAST_TYPE_NEVER:
        return NULL;

    case EAST_TYPE_NULL:
        return east_null();

    case EAST_TYPE_BOOLEAN: {
        if (*offset >= len) return NULL;
        bool val = data[(*offset)++] != 0;
        return east_boolean(val);
    }

    case EAST_TYPE_INTEGER: {
        int64_t val = read_zigzag(data, offset);
        return east_integer(val);
    }

    case EAST_TYPE_FLOAT: {
        if (*offset + 8 > len) return NULL;
        double val = read_float64_le(data, offset);
        return east_float(val);
    }

    case EAST_TYPE_STRING: {
        size_t slen;
        char *str = read_string_varint(data, len, offset, &slen);
        if (!str) return NULL;
        EastValue *val = east_string_len(str, slen);
        free(str);
        return val;
    }

    case EAST_TYPE_DATETIME: {
        int64_t millis = read_zigzag(data, offset);
        return east_datetime(millis);
    }

    case EAST_TYPE_BLOB: {
        uint64_t blen = read_varint(data, offset);
        if (*offset + blen > len) return NULL;
        EastValue *val = east_blob(data + *offset, (size_t)blen);
        *offset += (size_t)blen;
        return val;
    }

    case EAST_TYPE_ARRAY: {
        /* Backreference protocol */
        size_t pre_offset = *offset;
        uint64_t distance = read_varint(data, offset);
        if (distance > 0) {
            /* Backreference: look up value at (pre_offset - distance).
             * Use pre_offset (before reading varint) to match encoder which
             * computes distance from buf->len before writing the varint. */
            size_t ref_off = pre_offset - distance;
            EastValue *ref = beast2_dec_ctx_find(ctx, ref_off);
            if (ref) {
                ctx->backref_count++;
                east_value_retain(ref);
                return ref;
            }
            return NULL;
        }
        /* Inline: store offset, decode contents */
        size_t content_off = *offset;

        EastType *elem_type = type->data.element;
        uint64_t count = read_varint(data, offset);
        EastValue *arr = east_array_new(elem_type);
        if (!arr) return NULL;

        beast2_dec_ctx_add(ctx, arr, content_off);

        for (uint64_t i = 0; i < count; i++) {
            EastValue *elem = beast2_decode_value(data, len, offset, elem_type, ctx);
            if (!elem) { east_value_release(arr); return NULL; }
            east_array_push(arr, elem);
            east_value_release(elem);
        }
        return arr;
    }

    case EAST_TYPE_SET: {
        size_t pre_offset = *offset;
        uint64_t distance = read_varint(data, offset);
        if (distance > 0) {
            size_t ref_off = pre_offset - distance;
            EastValue *ref = beast2_dec_ctx_find(ctx, ref_off);
            if (ref) { ctx->backref_count++; east_value_retain(ref); return ref; }
            return NULL;
        }
        size_t content_off = *offset;

        EastType *elem_type = type->data.element;
        uint64_t count = read_varint(data, offset);
        EastValue *set = east_set_new(elem_type);
        if (!set) return NULL;

        beast2_dec_ctx_add(ctx, set, content_off);

        for (uint64_t i = 0; i < count; i++) {
            EastValue *elem = beast2_decode_value(data, len, offset, elem_type, ctx);
            if (!elem) { east_value_release(set); return NULL; }
            east_set_insert(set, elem);
            east_value_release(elem);
        }
        return set;
    }

    case EAST_TYPE_DICT: {
        size_t pre_offset = *offset;
        uint64_t distance = read_varint(data, offset);
        if (distance > 0) {
            size_t ref_off = pre_offset - distance;
            EastValue *ref = beast2_dec_ctx_find(ctx, ref_off);
            if (ref) { ctx->backref_count++; east_value_retain(ref); return ref; }
            return NULL;
        }
        size_t content_off = *offset;

        EastType *key_type = type->data.dict.key;
        EastType *val_type = type->data.dict.value;
        uint64_t count = read_varint(data, offset);
        EastValue *dict = east_dict_new(key_type, val_type);
        if (!dict) return NULL;

        beast2_dec_ctx_add(ctx, dict, content_off);

        for (uint64_t i = 0; i < count; i++) {
            EastValue *k = beast2_decode_value(data, len, offset, key_type, ctx);
            if (!k) { east_value_release(dict); return NULL; }
            EastValue *v = beast2_decode_value(data, len, offset, val_type, ctx);
            if (!v) { east_value_release(k); east_value_release(dict); return NULL; }
            east_dict_set(dict, k, v);
            east_value_release(k);
            east_value_release(v);
        }
        return dict;
    }

    case EAST_TYPE_STRUCT: {
        size_t dedup_start = *offset;
        int backref_before = ctx->backref_count;
        size_t nf = type->data.struct_.num_fields;
        const char **names = malloc(nf * sizeof(char *));
        EastValue **values = malloc(nf * sizeof(EastValue *));
        if (!names || !values) {
            free(names);
            free(values);
            return NULL;
        }

        for (size_t i = 0; i < nf; i++) {
            names[i] = type->data.struct_.fields[i].name;
            EastType *ftype = type->data.struct_.fields[i].type;
            values[i] = beast2_decode_value(data, len, offset, ftype, ctx);
            if (!values[i]) {
                for (size_t j = 0; j < i; j++) {
                    east_value_release(values[j]);
                }
                free(names);
                free(values);
                return NULL;
            }
        }

        /* Dedup: check if identical bytes were decoded before under this type.
         * Skip dedup if any backreferences were resolved during field decoding,
         * because backref distances are relative to buffer position — identical
         * bytes at different positions would resolve to different targets. */
        int had_backref = (ctx->backref_count != backref_before);
        size_t dedup_len = *offset - dedup_start;
#ifndef BEAST2_NO_DEDUP
#ifdef BEAST2_PROFILE_DEDUP
        double t_start = beast2_clock_us();
#endif
        uint64_t dedup_hash = hash_byte_range(data + dedup_start, dedup_len, (uintptr_t)type);
        ctx->dedup_bytes_hashed += dedup_len;
        if (!had_backref) {
            EastValue *cached = beast2_dedup_find(ctx, dedup_hash, data, dedup_start, dedup_len, type);
            if (cached) {
#ifdef BEAST2_PROFILE_DEDUP
                double elapsed = beast2_clock_us() - t_start;
                typeof(ctx->type_stats[0]) *ts = beast2_type_stats_get(ctx, type);
                ts->hits++;
                ts->bytes_hashed += dedup_len;
                ts->time_us += elapsed;
#endif
                ctx->dedup_hits++;
                for (size_t i = 0; i < nf; i++)
                    east_value_release(values[i]);
                free(names);
                free(values);
                east_value_retain(cached);
                return cached;
            }
        }
#ifdef BEAST2_PROFILE_DEDUP
        {
            double elapsed = beast2_clock_us() - t_start;
            typeof(ctx->type_stats[0]) *ts = beast2_type_stats_get(ctx, type);
            ts->misses++;
            ts->bytes_hashed += dedup_len;
            ts->time_us += elapsed;
        }
#endif
        ctx->dedup_misses++;
#endif

        EastValue *result = east_struct_new(names, values, nf, type);
        for (size_t i = 0; i < nf; i++) {
            east_value_release(values[i]);
        }
        free(names);
        free(values);
#ifndef BEAST2_NO_DEDUP
        if (!had_backref)
            beast2_dedup_add(ctx, dedup_hash, dedup_start, dedup_len, type, result);
#endif
        return result;
    }

    case EAST_TYPE_VARIANT: {
        size_t dedup_start = *offset;
        int backref_before = ctx->backref_count;
        uint64_t case_idx = read_varint(data, offset);
        if (case_idx >= type->data.variant.num_cases) return NULL;

        const char *case_name = type->data.variant.cases[case_idx].name;
        EastType *case_type = type->data.variant.cases[case_idx].type;

        EastValue *case_value = beast2_decode_value(data, len, offset, case_type, ctx);
        if (!case_value) return NULL;

        /* Dedup: check if identical bytes were decoded before under this type.
         * Skip when backreferences were resolved (same reason as struct). */
        int had_backref = (ctx->backref_count != backref_before);
        size_t dedup_len = *offset - dedup_start;
#ifndef BEAST2_NO_DEDUP
#ifdef BEAST2_PROFILE_DEDUP
        double vt_start = beast2_clock_us();
#endif
        uint64_t dedup_hash = hash_byte_range(data + dedup_start, dedup_len, (uintptr_t)type);
        ctx->dedup_bytes_hashed += dedup_len;
        if (!had_backref) {
            EastValue *cached = beast2_dedup_find(ctx, dedup_hash, data, dedup_start, dedup_len, type);
            if (cached) {
#ifdef BEAST2_PROFILE_DEDUP
                double elapsed = beast2_clock_us() - vt_start;
                typeof(ctx->type_stats[0]) *ts = beast2_type_stats_get(ctx, type);
                ts->hits++;
                ts->bytes_hashed += dedup_len;
                ts->time_us += elapsed;
#endif
                ctx->dedup_hits++;
                east_value_release(case_value);
                east_value_retain(cached);
                return cached;
            }
        }
#ifdef BEAST2_PROFILE_DEDUP
        {
            double elapsed = beast2_clock_us() - vt_start;
            typeof(ctx->type_stats[0]) *ts = beast2_type_stats_get(ctx, type);
            ts->misses++;
            ts->bytes_hashed += dedup_len;
            ts->time_us += elapsed;
        }
#endif
        ctx->dedup_misses++;
#endif

        EastValue *result = east_variant_new(case_name, case_value, type);
        east_value_release(case_value);
#ifndef BEAST2_NO_DEDUP
        if (!had_backref)
            beast2_dedup_add(ctx, dedup_hash, dedup_start, dedup_len, type, result);
#endif
        return result;
    }

    case EAST_TYPE_REF: {
        /* Ref also uses backreference protocol */
        size_t pre_offset = *offset;
        uint64_t distance = read_varint(data, offset);
        if (distance > 0) {
            size_t ref_off = pre_offset - distance;
            EastValue *ref = beast2_dec_ctx_find(ctx, ref_off);
            if (ref) { ctx->backref_count++; east_value_retain(ref); return ref; }
            return NULL;
        }
        size_t content_off = *offset;

        EastType *inner_type = type->data.element;
        EastValue *inner = beast2_decode_value(data, len, offset, inner_type, ctx);
        if (!inner) return NULL;
        EastValue *ref = east_ref_new(inner);
        east_value_release(inner);

        beast2_dec_ctx_add(ctx, ref, content_off);
        return ref;
    }

    case EAST_TYPE_VECTOR: {
        EastType *elem_type = type->data.element;
        uint64_t vlen = read_varint(data, offset);

        EastValue *vec = east_vector_new(elem_type, (size_t)vlen);
        if (!vec) return NULL;

        size_t elem_size = 0;
        if (elem_type->kind == EAST_TYPE_FLOAT) {
            elem_size = sizeof(double);
        } else if (elem_type->kind == EAST_TYPE_INTEGER) {
            elem_size = sizeof(int64_t);
        } else if (elem_type->kind == EAST_TYPE_BOOLEAN) {
            elem_size = sizeof(bool);
        }

        size_t byte_count = (size_t)vlen * elem_size;
        if (*offset + byte_count > len) {
            east_value_release(vec);
            return NULL;
        }
        memcpy(vec->data.vector.data, data + *offset, byte_count);
        *offset += byte_count;
        return vec;
    }

    case EAST_TYPE_MATRIX: {
        EastType *elem_type = type->data.element;
        uint64_t rows = read_varint(data, offset);
        uint64_t cols = read_varint(data, offset);

        EastValue *mat = east_matrix_new(elem_type, (size_t)rows, (size_t)cols);
        if (!mat) return NULL;

        size_t elem_size = 0;
        if (elem_type->kind == EAST_TYPE_FLOAT) {
            elem_size = sizeof(double);
        } else if (elem_type->kind == EAST_TYPE_INTEGER) {
            elem_size = sizeof(int64_t);
        } else if (elem_type->kind == EAST_TYPE_BOOLEAN) {
            elem_size = sizeof(bool);
        }

        size_t byte_count = (size_t)(rows * cols) * elem_size;
        if (*offset + byte_count > len) {
            east_value_release(mat);
            return NULL;
        }
        memcpy(mat->data.matrix.data, data + *offset, byte_count);
        *offset += byte_count;
        return mat;
    }

    case EAST_TYPE_RECURSIVE:
        if (type->data.recursive.node) {
            return beast2_decode_value(data, len, offset, type->data.recursive.node, ctx);
        }
        return NULL;

    case EAST_TYPE_FUNCTION:
    case EAST_TYPE_ASYNC_FUNCTION: {
        /* Ensure IR type is initialized */
        if (!east_ir_type) east_type_of_type_init();

        /* 1. Decode IR variant value (with type table restoration) */
        EastValue *ir_value;
        if (ctx->global_type_table) {
            Beast2DecodeCtx ir_dctx;
            beast2_dec_ctx_init(&ir_dctx);
            EastValue *indexed_ir = beast2_decode_value(data, len, offset,
                                                         east_ir_type_with_refs, &ir_dctx);
            beast2_dec_ctx_free(&ir_dctx);
            if (!indexed_ir) return NULL;
            RestoreCtx rctx = { ctx->global_type_table, ctx->global_type_table_size };
            ir_value = transform_type_positions(indexed_ir, east_ir_type, east_type_type,
                                                 restore_type_cb, &rctx);
            east_value_release(indexed_ir);
        } else {
            ir_value = beast2_decode_value(data, len, offset, east_ir_type, ctx);
        }
        if (!ir_value) return NULL;

        /* 2. Extract captures array from decoded IR */
        EastValue *fn_struct = ir_value->data.variant.value;
        EastValue *caps_arr = east_struct_get_field_idx(fn_struct, 2); /* captures */
        size_t ir_ncaps = (caps_arr && caps_arr->kind == EAST_VAL_ARRAY) ? caps_arr->data.array.len : 0;

        /* 3. Read capture count and validate */
        uint64_t ncaps = read_varint(data, offset);
        if (ncaps != ir_ncaps) {
            east_value_release(ir_value);
            return NULL;
        }

        /* 4. Create captures environment and decode each capture value */
        Environment *captures_env = env_new(NULL);

        for (uint64_t i = 0; i < ncaps; i++) {
            EastValue *cap_var = caps_arr->data.array.items[i];
            EastValue *cap_s = cap_var->data.variant.value;
            EastValue *name_v = east_struct_get_field_idx(cap_s, 2); /* name */
            EastValue *type_v = east_struct_get_field_idx(cap_s, 0); /* type */
            bool is_mutable = false;
            EastValue *mut_v = east_struct_get_field_idx(cap_s, 3); /* mutable */
            if (mut_v && mut_v->kind == EAST_VAL_BOOLEAN) is_mutable = mut_v->data.boolean;

            const char *cap_name = name_v->data.string.data;
            EastType *cap_type = east_type_from_value(type_v);

            EastValue *cap_val = beast2_decode_value(data, len, offset, cap_type, ctx);
            if (cap_type) east_type_release(cap_type);
            if (!cap_val) {
                env_release(captures_env);
                east_value_release(ir_value);
                return NULL;
            }

            /* Store capture value directly in environment.
             * The C compiler uses env_update for mutable captures (no Ref
             * wrapping), so we store all captures the same way. */
            env_set(captures_env, cap_name, cap_val);
            east_value_release(cap_val);
        }

        /* 5. Convert decoded IR to IRNode */
        IRNode *ir_node = east_ir_from_value(ir_value);
        if (!ir_node) {
            env_release(captures_env);
            east_value_release(ir_value);
            return NULL;
        }

        /* 6. Build EastCompiledFn */
        EastCompiledFn *fn = calloc(1, sizeof(EastCompiledFn));
        if (!fn) {
            ir_node_release(ir_node);
            env_release(captures_env);
            east_value_release(ir_value);
            return NULL;
        }

        fn->ir = ir_node->data.function.body;
        ir_node_retain(fn->ir);
        fn->captures = captures_env;
        fn->num_params = ir_node->data.function.num_params;
        if (fn->num_params > 0) {
            fn->param_names = calloc(fn->num_params, sizeof(char *));
            for (size_t i = 0; i < fn->num_params; i++) {
                fn->param_names[i] = strdup(ir_node->data.function.params[i].name);
            }
        }
        fn->platform = east_current_platform();
        fn->builtins = east_current_builtins();
        fn->source_ir = ir_value; /* already retained from decode */

        ir_node_release(ir_node);

        EastValue *result = east_function_value(fn);
        return result;
    }
    }

    return NULL;
}

EastValue *east_beast2_decode(const uint8_t *data, size_t len, EastType *type)
{
    if (!data || !type) return NULL;
    size_t offset = 0;
    Beast2DecodeCtx ctx;
    beast2_dec_ctx_init(&ctx);
    EastValue *result = beast2_decode_value(data, len, &offset, type, &ctx);
    beast2_dec_ctx_free(&ctx);
    return result;
}

/* ================================================================== */
/*  BEAST2 Type Schema Encoding/Decoding                               */
/*                                                                     */
/*  The type schema in the full format is a beast2-encoded value of    */
/*  east_type_type (EastTypeType).  We use east_type_to_value to       */
/*  convert EastType* -> EastValue*, then encode/decode it with the    */
/*  standard beast2 value codec.  This matches the TypeScript impl.    */
/* ================================================================== */

/* ================================================================== */
/*  BEAST2 Full-Format Encode/Decode (header + type schema + value)    */
/* ================================================================== */

static const uint8_t BEAST2_MAGIC[8] = {
    0x89, 0x45, 0x61, 0x73, 0x74, 0x0D, 0x0A, 0x01
};

ByteBuffer *east_beast2_encode_full(EastValue *value, EastType *type)
{
    if (!value || !type) return NULL;

    /* Ensure type system is initialized */
    if (!east_type_type) east_type_of_type_init();

    ByteBuffer *buf = byte_buffer_new(256);
    if (!buf) return NULL;

    /* 1. Write magic bytes */
    byte_buffer_write_bytes(buf, BEAST2_MAGIC, 8);

    /* 2. Write type schema as a beast2-encoded EastTypeType value */
    EastValue *type_val = east_type_to_value(type);
    if (type_val) {
        Beast2EncodeCtx schema_ctx;
        beast2_enc_ctx_init(&schema_ctx);
        beast2_encode_value(buf, type_val, east_type_type, &schema_ctx);
        beast2_enc_ctx_free(&schema_ctx);
        east_value_release(type_val);
    }

    /* 3. Pre-scan for functions and write global type table */
    Beast2TypeTable type_table;
    type_table_init(&type_table);
    pre_scan_for_functions(value, type, &type_table);

    write_varint(buf, (uint64_t)type_table.count);
    for (size_t i = 0; i < type_table.count; i++) {
        Beast2EncodeCtx tt_ctx;
        beast2_enc_ctx_init(&tt_ctx);
        beast2_encode_value(buf, type_table.types[i], east_type_type, &tt_ctx);
        beast2_enc_ctx_free(&tt_ctx);
    }

    /* 4. Write value data (with type table in context) */
    Beast2EncodeCtx ctx;
    beast2_enc_ctx_init(&ctx);
    ctx.global_type_table = &type_table;
    beast2_encode_value(buf, value, type, &ctx);
    beast2_enc_ctx_free(&ctx);
    type_table_free(&type_table);

    return buf;
}

ByteBuffer *east_beast2_encode_full_with_handles(EastValue *value, EastType *type,
                                                  Beast2HandleAllocFn alloc_fn, void *user_data)
{
    if (!value || !type || !alloc_fn) return NULL;

    if (!east_type_type) east_type_of_type_init();

    ByteBuffer *buf = byte_buffer_new(256);
    if (!buf) return NULL;

    /* 1. Write magic bytes */
    byte_buffer_write_bytes(buf, BEAST2_MAGIC, 8);

    /* 2. Write type schema (same as standard encode) */
    EastValue *type_val = east_type_to_value(type);
    if (type_val) {
        Beast2EncodeCtx schema_ctx;
        beast2_enc_ctx_init(&schema_ctx);
        beast2_encode_value(buf, type_val, east_type_type, &schema_ctx);
        beast2_enc_ctx_free(&schema_ctx);
        east_value_release(type_val);
    }

    /* 3. Write empty global type table (handles don't need it) */
    write_varint(buf, 0);

    /* 4. Write value data with handle-aware encoder */
    Beast2EncodeCtx ctx;
    beast2_enc_ctx_init(&ctx);
    ctx.fn_handle_alloc = alloc_fn;
    ctx.fn_handle_user_data = user_data;
    beast2_encode_value(buf, value, type, &ctx);
    beast2_enc_ctx_free(&ctx);

    return buf;
}

EastValue *east_beast2_decode_full(const uint8_t *data, size_t len,
                                   EastType *type)
{
    if (!data || !type) return NULL;
    if (len < 8) return NULL;

    /* 1. Verify magic bytes */
    if (memcmp(data, BEAST2_MAGIC, 8) != 0) return NULL;

    /* Ensure type system is initialized */
    if (!east_type_type) east_type_of_type_init();

    size_t offset = 8;

    /* 2. Decode type schema (advances offset past the schema bytes).
     *    The schema is a beast2-encoded EastTypeType value. */
    Beast2DecodeCtx schema_ctx;
    beast2_dec_ctx_init(&schema_ctx);
    EastValue *schema_val = beast2_decode_value(data, len, &offset,
                                                 east_type_type, &schema_ctx);
    beast2_dec_ctx_free(&schema_ctx);
    if (schema_val) east_value_release(schema_val);
    else return NULL;

    /* 3. Read global type table */
    uint64_t table_size = read_varint(data, &offset);
    EastValue **global_tt = NULL;
    if (table_size > 0) {
        global_tt = malloc((size_t)table_size * sizeof(EastValue*));
        for (uint64_t i = 0; i < table_size; i++) {
            Beast2DecodeCtx tt_ctx;
            beast2_dec_ctx_init(&tt_ctx);
            global_tt[i] = beast2_decode_value(data, len, &offset, east_type_type, &tt_ctx);
            beast2_dec_ctx_free(&tt_ctx);
            if (!global_tt[i]) {
                for (uint64_t j = 0; j < i; j++) east_value_release(global_tt[j]);
                free(global_tt);
                return NULL;
            }
        }
    }

    /* 4. Decode value from remaining data using the provided type */
    Beast2DecodeCtx dctx;
    beast2_dec_ctx_init(&dctx);
    dctx.global_type_table = global_tt;
    dctx.global_type_table_size = (size_t)table_size;
    EastValue *result = beast2_decode_value(data, len, &offset, type, &dctx);
    beast2_dec_ctx_free(&dctx);

    /* Free type table */
    for (uint64_t i = 0; i < table_size; i++) east_value_release(global_tt[i]);
    free(global_tt);

    if (!result) return NULL;

    /* 5. Verify all bytes consumed — leftover bytes indicate a type mismatch */
    if (offset != len) {
        east_value_release(result);
        return NULL;
    }
    return result;
}

EastValue *east_beast2_decode_auto(const uint8_t *data, size_t len)
{
    if (!data) return NULL;
    if (len < 8) return NULL;

    /* 1. Verify magic bytes */
    if (memcmp(data, BEAST2_MAGIC, 8) != 0) return NULL;

    /* Ensure type system is initialized */
    if (!east_type_type) east_type_of_type_init();

    size_t offset = 8;

    /* 2. Decode type schema and convert to EastType* */
    Beast2DecodeCtx schema_ctx;
    beast2_dec_ctx_init(&schema_ctx);
    EastValue *schema_val = beast2_decode_value(data, len, &offset,
                                                 east_type_type, &schema_ctx);
    beast2_dec_ctx_free(&schema_ctx);
    if (!schema_val) return NULL;

    EastType *type = east_type_from_value(schema_val);
    east_value_release(schema_val);
    if (!type) return NULL;

    /* 3. Read global type table */
    uint64_t table_size = read_varint(data, &offset);
    EastValue **global_tt = NULL;
    if (table_size > 0) {
        global_tt = malloc((size_t)table_size * sizeof(EastValue*));
        for (uint64_t i = 0; i < table_size; i++) {
            Beast2DecodeCtx tt_ctx;
            beast2_dec_ctx_init(&tt_ctx);
            global_tt[i] = beast2_decode_value(data, len, &offset, east_type_type, &tt_ctx);
            beast2_dec_ctx_free(&tt_ctx);
            if (!global_tt[i]) {
                for (uint64_t j = 0; j < i; j++) east_value_release(global_tt[j]);
                free(global_tt);
                east_type_release(type);
                return NULL;
            }
        }
    }

    /* 4. Decode value using the extracted type */
    Beast2DecodeCtx dctx;
    beast2_dec_ctx_init(&dctx);
    dctx.global_type_table = global_tt;
    dctx.global_type_table_size = (size_t)table_size;
    EastValue *result = beast2_decode_value(data, len, &offset, type, &dctx);
#ifdef BEAST2_PROFILE_DEDUP
    beast2_dedup_print_stats(&dctx);
#endif
    beast2_dec_ctx_free(&dctx);
    east_type_release(type);

    for (uint64_t i = 0; i < table_size; i++) east_value_release(global_tt[i]);
    free(global_tt);

    if (!result) return NULL;

    /* 5. Verify all bytes consumed */
    if (offset != len) {
        east_value_release(result);
        return NULL;
    }
    return result;
}

EastType *east_beast2_extract_type(const uint8_t *data, size_t len)
{
    if (!data || len < 8) return NULL;
    if (memcmp(data, BEAST2_MAGIC, 8) != 0) return NULL;

    if (!east_type_type) east_type_of_type_init();

    size_t offset = 8;
    Beast2DecodeCtx schema_ctx;
    beast2_dec_ctx_init(&schema_ctx);
    EastValue *schema_val = beast2_decode_value(data, len, &offset,
                                                 east_type_type, &schema_ctx);
    beast2_dec_ctx_free(&schema_ctx);
    if (!schema_val) return NULL;

    EastType *type = east_type_from_value(schema_val);
    east_value_release(schema_val);
    return type;
}
