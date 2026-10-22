#include "rng.h"

#include <assert.h>
#include <math.h>

int rng_init(struct rng *rng, uint32_t seed) {
    if (seed == 0) {
        return -1;
    }
    rng->state = seed;
    return 0;
}

uint32_t rng_next(struct rng *rng) {
    uint32_t x = rng->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng->state = x;
    return x;
}

double rng_uniform01(struct rng *rng) {
    uint32_t raw = rng_next(rng);
    return ((double)raw + 1.0) / 4294967296.0;   /* 2^32; nunca 0, puede dar 1.0 */
}

double rng_exponential_ms(struct rng *rng, double mean_ms) {
    assert(mean_ms > 0.0);   /* --mean-ms 0 lo maneja el llamador sin invocar esto */

    double u = rng_uniform01(rng);
    return -mean_ms * log(u);
}
