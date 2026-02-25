/*
 * Random number generation platform functions for East.
 *
 * Provides random number generation for East programs running in C.
 * Supports both /dev/urandom-backed random (default) and seedable PRNG
 * (XorShift128+) for reproducible simulations.
 */

#include "east_std/east_std.h"
#include <east/values.h>
#include <east/eval_result.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ========================================================================
 * XorShift128+ PRNG State
 * ======================================================================== */

typedef struct {
    uint64_t state0;
    uint64_t state1;
    bool seeded; /* true if explicitly seeded (use PRNG), false = use urandom */
} RNGState;

static RNGState rng_global = { 0, 0, false };

/* SplitMix64 for state initialization from seed */
static uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static void rng_seed(RNGState *rng, uint64_t seed) {
    rng->state0 = splitmix64(seed);
    rng->state1 = splitmix64(rng->state0);
    if (rng->state0 == 0 && rng->state1 == 0) {
        rng->state0 = 1;
    }
    rng->seeded = true;
}

static double rng_next_xorshift(RNGState *rng) {
    uint64_t s1 = rng->state0;
    uint64_t s0 = rng->state1;
    uint64_t result = s0 + s1;

    rng->state0 = s0;
    s1 ^= s1 << 23;
    rng->state1 = s1 ^ s0 ^ (s1 >> 17) ^ (s0 >> 26);

    /* Convert to [0, 1) using upper 53 bits */
    uint64_t upper53 = (result >> 11) & ((1ULL << 53) - 1);
    return (double)upper53 / (double)(1ULL << 53);
}

static double rng_next_urandom(void) {
    uint64_t val;
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return 0.0;
    size_t n = fread(&val, sizeof(val), 1, f);
    fclose(f);
    if (n != 1) return 0.0;
    /* Use upper 53 bits for [0, 1) */
    uint64_t upper53 = (val >> 11) & ((1ULL << 53) - 1);
    return (double)upper53 / (double)(1ULL << 53);
}

static double rng_next(void) {
    if (rng_global.seeded) {
        return rng_next_xorshift(&rng_global);
    }
    return rng_next_urandom();
}

/* Seed the global state from /dev/urandom (lazy initialization for xorshift if needed) */
static void rng_ensure_init(void) {
    /* Nothing to do for urandom mode */
    (void)0;
}

/* ========================================================================
 * Platform Functions
 * ======================================================================== */

static EvalResult random_seed(EastValue **args, size_t num_args) {
    (void)num_args;
    int64_t seed = args[0]->data.integer;
    rng_seed(&rng_global, (uint64_t)seed);
    return eval_ok(east_null());
}

static EvalResult random_uniform(EastValue **args, size_t num_args) {
    (void)args;
    (void)num_args;
    rng_ensure_init();
    return eval_ok(east_float(rng_next()));
}

static EvalResult random_normal(EastValue **args, size_t num_args) {
    (void)args;
    (void)num_args;
    rng_ensure_init();

    /* Marsaglia polar method (Box-Muller) */
    double u, v, s;
    do {
        u = 2.0 * rng_next() - 1.0;
        v = 2.0 * rng_next() - 1.0;
        s = u * u + v * v;
    } while (s <= 0.0 || s >= 1.0);

    return eval_ok(east_float(u * sqrt(-2.0 * log(s) / s)));
}

static EvalResult random_range(EastValue **args, size_t num_args) {
    (void)num_args;
    rng_ensure_init();

    int64_t min_val = args[0]->data.integer;
    int64_t max_val = args[1]->data.integer;

    if (min_val > max_val) {
        return eval_error("Invalid range");
    }

    int64_t range_size = max_val - min_val + 1;
    int64_t result = (int64_t)(rng_next() * (double)range_size) + min_val;
    return eval_ok(east_integer(result));
}

static EvalResult random_exponential(EastValue **args, size_t num_args) {
    (void)num_args;
    rng_ensure_init();

    double lambda_rate = args[0]->data.float64;
    if (lambda_rate <= 0.0) {
        return eval_ok(east_float(0.0));
    }

    double u = rng_next();
    return eval_ok(east_float(-log(1.0 - u) / lambda_rate));
}

static EvalResult random_weibull(EastValue **args, size_t num_args) {
    (void)num_args;
    rng_ensure_init();

    double shape_k = args[0]->data.float64;
    if (shape_k <= 0.0) {
        return eval_ok(east_float(0.0));
    }

    double u = rng_next();
    return eval_ok(east_float(pow(-log(1.0 - u), 1.0 / shape_k)));
}

static EvalResult random_pareto(EastValue **args, size_t num_args) {
    (void)num_args;
    rng_ensure_init();

    double alpha = args[0]->data.float64;
    if (alpha <= 0.0) {
        return eval_ok(east_float(1.0));
    }

    double u = rng_next();
    return eval_ok(east_float(pow(1.0 - u, -1.0 / alpha)));
}

static EvalResult random_log_normal(EastValue **args, size_t num_args) {
    (void)num_args;
    rng_ensure_init();

    double mu = args[0]->data.float64;
    double sigma = args[1]->data.float64;

    if (sigma <= 0.0) {
        return eval_ok(east_float(exp(mu)));
    }

    /* Generate standard normal using Marsaglia polar method */
    double u, v, s;
    do {
        u = 2.0 * rng_next() - 1.0;
        v = 2.0 * rng_next() - 1.0;
        s = u * u + v * v;
    } while (s <= 0.0 || s >= 1.0);

    double z = u * sqrt(-2.0 * log(s) / s);
    return eval_ok(east_float(exp(mu + sigma * z)));
}

static EvalResult random_irwin_hall(EastValue **args, size_t num_args) {
    (void)num_args;
    rng_ensure_init();

    int64_t n = args[0]->data.integer;
    if (n <= 0) {
        return eval_ok(east_float(0.0));
    }

    double sum = 0.0;
    for (int64_t i = 0; i < n; i++) {
        sum += rng_next();
    }
    return eval_ok(east_float(sum));
}

static EvalResult random_bates(EastValue **args, size_t num_args) {
    (void)num_args;
    rng_ensure_init();

    int64_t n = args[0]->data.integer;
    if (n <= 0) {
        return eval_ok(east_float(0.0));
    }

    double sum = 0.0;
    for (int64_t i = 0; i < n; i++) {
        sum += rng_next();
    }
    return eval_ok(east_float(sum / (double)n));
}

static EvalResult random_bernoulli(EastValue **args, size_t num_args) {
    (void)num_args;
    rng_ensure_init();

    double p = args[0]->data.float64;
    return eval_ok(east_integer(rng_next() < p ? 1 : 0));
}

static EvalResult random_binomial(EastValue **args, size_t num_args) {
    (void)num_args;
    rng_ensure_init();

    int64_t n = args[0]->data.integer;
    double p = args[1]->data.float64;

    if (n < 0) return eval_ok(east_integer(0));

    int64_t count = 0;
    for (int64_t i = 0; i < n; i++) {
        if (rng_next() < p) {
            count++;
        }
    }
    return eval_ok(east_integer(count));
}

static EvalResult random_geometric(EastValue **args, size_t num_args) {
    (void)num_args;
    rng_ensure_init();

    double p = args[0]->data.float64;
    if (p <= 0.0 || p > 1.0) {
        return eval_ok(east_integer(1));
    }

    double u = rng_next();
    return eval_ok(east_integer((int64_t)ceil(log(1.0 - u) / log(1.0 - p))));
}

static EvalResult random_poisson(EastValue **args, size_t num_args) {
    (void)num_args;
    rng_ensure_init();

    double lambda_rate = args[0]->data.float64;
    if (lambda_rate < 0.0) return eval_ok(east_integer(0));
    if (lambda_rate == 0.0) return eval_ok(east_integer(0));

    if (lambda_rate < 30.0) {
        /* Knuth algorithm for small lambda */
        double limit_l = exp(-lambda_rate);
        int64_t k = 0;
        double p = 1.0;
        do {
            k++;
            p *= rng_next();
        } while (p > limit_l);
        return eval_ok(east_integer(k - 1));
    } else {
        /* Normal approximation for large lambda */
        double u, v, s;
        do {
            u = 2.0 * rng_next() - 1.0;
            v = 2.0 * rng_next() - 1.0;
            s = u * u + v * v;
        } while (s <= 0.0 || s >= 1.0);

        double z = u * sqrt(-2.0 * log(s) / s);
        int64_t result = (int64_t)(z * sqrt(lambda_rate) + lambda_rate);
        if (result < 0) result = 0;
        return eval_ok(east_integer(result));
    }
}

void east_std_register_random(PlatformRegistry *reg) {
    platform_registry_add(reg, "random_seed", random_seed, false);
    platform_registry_add(reg, "random_uniform", random_uniform, false);
    platform_registry_add(reg, "random_normal", random_normal, false);
    platform_registry_add(reg, "random_range", random_range, false);
    platform_registry_add(reg, "random_exponential", random_exponential, false);
    platform_registry_add(reg, "random_weibull", random_weibull, false);
    platform_registry_add(reg, "random_pareto", random_pareto, false);
    platform_registry_add(reg, "random_log_normal", random_log_normal, false);
    platform_registry_add(reg, "random_irwin_hall", random_irwin_hall, false);
    platform_registry_add(reg, "random_bates", random_bates, false);
    platform_registry_add(reg, "random_bernoulli", random_bernoulli, false);
    platform_registry_add(reg, "random_binomial", random_binomial, false);
    platform_registry_add(reg, "random_geometric", random_geometric, false);
    platform_registry_add(reg, "random_poisson", random_poisson, false);
}
