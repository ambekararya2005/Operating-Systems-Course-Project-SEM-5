/*
 * OS concept: Deterministic workload generation for reproducible experiments — Unit 2
 *
 * splitmix64: tiny, portable PRNG. Same seed => same sequence on every machine,
 * unlike rand() whose algorithm is implementation-defined.
 */
#ifndef RELIEF_RNG_H
#define RELIEF_RNG_H

#include <stdint.h>

typedef struct {
    uint64_t state;
} rng_t;

static inline void rng_seed(rng_t *r, uint64_t seed)
{
    r->state = seed;
}

static inline uint64_t rng_next(rng_t *r)
{
    uint64_t z = (r->state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Uniform integer in [lo, hi]. Modulo bias is negligible for our small ranges. */
static inline uint32_t rng_range(rng_t *r, uint32_t lo, uint32_t hi)
{
    return lo + (uint32_t)(rng_next(r) % ((uint64_t)hi - lo + 1));
}

#endif /* RELIEF_RNG_H */
