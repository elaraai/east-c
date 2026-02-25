/*
 * Binary utilities for East serialization.
 *
 * Provides ByteBuffer for managed binary writing, and varint/zigzag
 * encoding/decoding for variable-length integer representation.
 */

#include "east/serialization.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  ByteBuffer                                                         */
/* ------------------------------------------------------------------ */

ByteBuffer *byte_buffer_new(size_t initial_cap)
{
    ByteBuffer *buf = malloc(sizeof(ByteBuffer));
    if (!buf) return NULL;

    if (initial_cap == 0) initial_cap = 256;

    buf->data = malloc(initial_cap);
    if (!buf->data) {
        free(buf);
        return NULL;
    }
    buf->len = 0;
    buf->cap = initial_cap;
    return buf;
}

void byte_buffer_free(ByteBuffer *buf)
{
    if (!buf) return;
    free(buf->data);
    free(buf);
}

static void byte_buffer_ensure_capacity(ByteBuffer *buf, size_t needed)
{
    size_t required = buf->len + needed;
    if (required <= buf->cap) return;

    /* Exponential growth: at least double, but at least required */
    size_t new_cap = buf->cap * 2;
    if (new_cap < required) new_cap = required;

    /* Cap growth at +1GB per resize */
    size_t max_growth = buf->cap + (size_t)1024 * 1024 * 1024;
    if (new_cap > max_growth && max_growth >= required) {
        new_cap = max_growth;
    }

    uint8_t *new_data = realloc(buf->data, new_cap);
    if (!new_data) {
        /* Allocation failure -- best effort, caller should check */
        return;
    }
    buf->data = new_data;
    buf->cap = new_cap;
}

void byte_buffer_write_u8(ByteBuffer *buf, uint8_t val)
{
    byte_buffer_ensure_capacity(buf, 1);
    buf->data[buf->len++] = val;
}

void byte_buffer_write_bytes(ByteBuffer *buf, const uint8_t *data, size_t len)
{
    if (len == 0) return;
    byte_buffer_ensure_capacity(buf, len);
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
}

/* ------------------------------------------------------------------ */
/*  Varint encoding (unsigned LEB128)                                  */
/* ------------------------------------------------------------------ */

void write_varint(ByteBuffer *buf, uint64_t val)
{
    while (val >= 0x80) {
        byte_buffer_write_u8(buf, (uint8_t)((val & 0x7F) | 0x80));
        val >>= 7;
    }
    byte_buffer_write_u8(buf, (uint8_t)(val & 0x7F));
}

uint64_t read_varint(const uint8_t *data, size_t *offset)
{
    uint64_t result = 0;
    int shift = 0;
    size_t pos = *offset;

    for (;;) {
        uint8_t byte = data[pos++];
        result |= (uint64_t)(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            break;
        }
        shift += 7;
        if (shift >= 64) {
            /* Overflow protection */
            break;
        }
    }

    *offset = pos;
    return result;
}

/* ------------------------------------------------------------------ */
/*  Zigzag encoding (signed -> unsigned mapping)                       */
/*                                                                     */
/*  Maps: 0->0, -1->1, 1->2, -2->3, 2->4, ...                       */
/*  Formula: encode = (n << 1) ^ (n >> 63)                            */
/*  Formula: decode = (n >> 1) ^ -(n & 1)                             */
/* ------------------------------------------------------------------ */

void write_zigzag(ByteBuffer *buf, int64_t val)
{
    uint64_t zigzag = ((uint64_t)val << 1) ^ (uint64_t)(val >> 63);
    write_varint(buf, zigzag);
}

int64_t read_zigzag(const uint8_t *data, size_t *offset)
{
    uint64_t raw = read_varint(data, offset);
    /* Decode zigzag: (raw >> 1) ^ -(raw & 1) */
    return (int64_t)((raw >> 1) ^ (~(raw & 1) + 1));
}
