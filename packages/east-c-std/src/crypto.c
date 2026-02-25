/*
 * Cryptographic platform functions for East.
 *
 * Provides cryptographic operations for East programs running in C.
 * Includes embedded SHA-256 implementation (no external dependencies).
 */

#include "east_std/east_std.h"
#include <east/values.h>
#include <east/eval_result.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ========================================================================
 * SHA-256 Implementation
 * ======================================================================== */

#define SHA256_BLOCK_SIZE 64
#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t buffer[SHA256_BLOCK_SIZE];
    size_t buffer_len;
} SHA256_CTX;

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static inline uint32_t sha256_rotr(uint32_t x, unsigned int n) {
    return (x >> n) | (x << (32 - n));
}

static inline uint32_t sha256_ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

static inline uint32_t sha256_maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

static inline uint32_t sha256_sigma0(uint32_t x) {
    return sha256_rotr(x, 2) ^ sha256_rotr(x, 13) ^ sha256_rotr(x, 22);
}

static inline uint32_t sha256_sigma1(uint32_t x) {
    return sha256_rotr(x, 6) ^ sha256_rotr(x, 11) ^ sha256_rotr(x, 25);
}

static inline uint32_t sha256_gamma0(uint32_t x) {
    return sha256_rotr(x, 7) ^ sha256_rotr(x, 18) ^ (x >> 3);
}

static inline uint32_t sha256_gamma1(uint32_t x) {
    return sha256_rotr(x, 17) ^ sha256_rotr(x, 19) ^ (x >> 10);
}

static void sha256_transform(SHA256_CTX *ctx, const uint8_t block[SHA256_BLOCK_SIZE]) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;

    /* Prepare message schedule */
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        w[i] = sha256_gamma1(w[i - 2]) + w[i - 7] +
               sha256_gamma0(w[i - 15]) + w[i - 16];
    }

    /* Initialize working variables */
    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    /* Compression function */
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + sha256_sigma1(e) + sha256_ch(e, f, g) + sha256_k[i] + w[i];
        uint32_t t2 = sha256_sigma0(a) + sha256_maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    /* Add compressed chunk to current hash value */
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(SHA256_CTX *ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->bit_count = 0;
    ctx->buffer_len = 0;
}

static void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len) {
    ctx->bit_count += (uint64_t)len * 8;

    while (len > 0) {
        size_t space = SHA256_BLOCK_SIZE - ctx->buffer_len;
        size_t copy = (len < space) ? len : space;

        memcpy(ctx->buffer + ctx->buffer_len, data, copy);
        ctx->buffer_len += copy;
        data += copy;
        len -= copy;

        if (ctx->buffer_len == SHA256_BLOCK_SIZE) {
            sha256_transform(ctx, ctx->buffer);
            ctx->buffer_len = 0;
        }
    }
}

static void sha256_final(SHA256_CTX *ctx, uint8_t digest[SHA256_DIGEST_SIZE]) {
    /* Pad message */
    ctx->buffer[ctx->buffer_len++] = 0x80;

    if (ctx->buffer_len > 56) {
        /* Not enough room for length, process this block and start a new one */
        memset(ctx->buffer + ctx->buffer_len, 0, SHA256_BLOCK_SIZE - ctx->buffer_len);
        sha256_transform(ctx, ctx->buffer);
        ctx->buffer_len = 0;
    }

    memset(ctx->buffer + ctx->buffer_len, 0, 56 - ctx->buffer_len);

    /* Append bit count (big-endian) */
    uint64_t bits = ctx->bit_count;
    ctx->buffer[56] = (uint8_t)(bits >> 56);
    ctx->buffer[57] = (uint8_t)(bits >> 48);
    ctx->buffer[58] = (uint8_t)(bits >> 40);
    ctx->buffer[59] = (uint8_t)(bits >> 32);
    ctx->buffer[60] = (uint8_t)(bits >> 24);
    ctx->buffer[61] = (uint8_t)(bits >> 16);
    ctx->buffer[62] = (uint8_t)(bits >> 8);
    ctx->buffer[63] = (uint8_t)(bits);

    sha256_transform(ctx, ctx->buffer);

    /* Produce digest (big-endian) */
    for (int i = 0; i < 8; i++) {
        digest[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

static void sha256_compute(const uint8_t *data, size_t len, uint8_t digest[SHA256_DIGEST_SIZE]) {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);
}

/* ========================================================================
 * Utility: Read from /dev/urandom
 * ======================================================================== */

static int read_urandom(uint8_t *buf, size_t len) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    return (n == len) ? 0 : -1;
}

/* ========================================================================
 * Platform Functions
 * ======================================================================== */

static EvalResult crypto_random_bytes(EastValue **args, size_t num_args) {
    (void)num_args;
    int64_t length = args[0]->data.integer;

    if (length <= 0) {
        return eval_ok(east_blob(NULL, 0));
    }

    uint8_t *buf = malloc((size_t)length);
    if (!buf) {
        return eval_ok(east_blob(NULL, 0));
    }

    if (read_urandom(buf, (size_t)length) != 0) {
        free(buf);
        return eval_ok(east_blob(NULL, 0));
    }

    EastValue *result = east_blob(buf, (size_t)length);
    free(buf);
    return eval_ok(result);
}

static EvalResult crypto_hash_sha256(EastValue **args, size_t num_args) {
    (void)num_args;
    const uint8_t *data = (const uint8_t *)args[0]->data.string.data;
    size_t len = args[0]->data.string.len;

    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256_compute(data, len, digest);

    /* Convert to hex string */
    char hex[SHA256_DIGEST_SIZE * 2 + 1];
    for (int i = 0; i < SHA256_DIGEST_SIZE; i++) {
        sprintf(hex + i * 2, "%02x", digest[i]);
    }
    hex[SHA256_DIGEST_SIZE * 2] = '\0';

    return eval_ok(east_string(hex));
}

static EvalResult crypto_hash_sha256_bytes(EastValue **args, size_t num_args) {
    (void)num_args;
    const uint8_t *data = args[0]->data.blob.data;
    size_t len = args[0]->data.blob.len;

    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256_compute(data, len, digest);

    return eval_ok(east_blob(digest, SHA256_DIGEST_SIZE));
}

static EvalResult crypto_uuid(EastValue **args, size_t num_args) {
    (void)args;
    (void)num_args;

    uint8_t bytes[16];
    if (read_urandom(bytes, 16) != 0) {
        return eval_ok(east_string("00000000-0000-0000-0000-000000000000"));
    }

    /* Set version 4 (bits 12-15 of time_hi_and_version) */
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    /* Set variant (bits 6-7 of clock_seq_hi_and_reserved) */
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    /* Format as UUID string: 8-4-4-4-12 */
    char uuid_str[37];
    snprintf(uuid_str, sizeof(uuid_str),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5],
             bytes[6], bytes[7],
             bytes[8], bytes[9],
             bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);

    return eval_ok(east_string(uuid_str));
}

void east_std_register_crypto(PlatformRegistry *reg) {
    platform_registry_add(reg, "crypto_random_bytes", crypto_random_bytes, false);
    platform_registry_add(reg, "crypto_hash_sha256", crypto_hash_sha256, false);
    platform_registry_add(reg, "crypto_hash_sha256_bytes", crypto_hash_sha256_bytes, false);
    platform_registry_add(reg, "crypto_uuid", crypto_uuid, false);
}
