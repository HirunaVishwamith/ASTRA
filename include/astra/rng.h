/* rng.h — deterministic PCG32 PRNG. The C sim cannot match Python's Mersenne
 * Twister bit-for-bit, so RNG-driven modules (failures, traffic) are verified
 * by invariants + reproducibility instead. PCG32 is fast, small-state, and
 * well-distributed. Ref: O'Neill, pcg-random.org. */
#ifndef ASTRA_RNG_H
#define ASTRA_RNG_H

#include <stdint.h>

typedef struct { uint64_t state, inc; } Rng;

static inline uint32_t rng_u32(Rng *r) {
    uint64_t old = r->state;
    r->state = old * 6364136223846793005ULL + (r->inc | 1u);
    uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31u));
}

static inline void rng_seed(Rng *r, uint64_t seed, uint64_t seq) {
    r->state = 0u; r->inc = (seq << 1u) | 1u;
    rng_u32(r); r->state += seed; rng_u32(r);
}

/* uniform double in [0,1) */
static inline double rng_double(Rng *r) {
    return (double)(rng_u32(r) >> 5) * (1.0 / 134217728.0); /* 2^-27 */
}

/* uniform int in [0,n) */
static inline uint32_t rng_below(Rng *r, uint32_t n) {
    uint32_t thresh = (uint32_t)(-n) % n;          /* unbiased rejection */
    for (;;) { uint32_t v = rng_u32(r); if (v >= thresh) return v % n; }
}

#endif /* ASTRA_RNG_H */
