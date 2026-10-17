/* metrics.c — histograma de latencia, estadistica entre repeticiones y
 * throughput (D11). El escritor del resumen (summary_write_csv) es del
 * Issue #18, no de este archivo.
 */
#include "metrics.h"

#include <assert.h>
#include <math.h>

/* floor(log2(n)) para n >= 1, con un ciclo portable (sin builtins de
 * compilador). Se llama una vez por extraccion, dentro de la region
 * critica de sync_get: el costo de hasta 64 iteraciones es despreciable
 * frente a la adquisicion del mutex (ver D11). */
static uint32_t floor_log2_u64(uint64_t n) {
    uint32_t e = 0;
    while (n > 1) {
        n >>= 1;
        e++;
    }
    return e;
}

uint32_t lat_hist_bucket(uint64_t ns) {
    if (ns < LAT_SUB_BUCKETS) {   /* ns < 8: una cubeta exacta por valor */
        return (uint32_t)ns;
    }

    uint32_t e = floor_log2_u64(ns);   /* e >= 3, porque ns >= 8 */
    assert(e >= LAT_SUB_BITS);   /* si no, el shift de abajo es UB (negativo) */

    uint32_t sub = (uint32_t)((ns >> (e - LAT_SUB_BITS)) & (LAT_SUB_BUCKETS - 1));
    uint32_t bucket = (e - 2u) * LAT_SUB_BUCKETS + sub;

    /* bucket alimenta directo un indice de arreglo en memoria compartida
     * (h->buckets[...] en lat_hist_add); fuera de rango ahi no crashea
     * limpio, corrompe en silencio lo que este al lado. */
    assert(bucket < LAT_BUCKETS);
    return bucket;
}

uint64_t lat_hist_bucket_mid(uint32_t bucket) {
    assert(bucket < LAT_BUCKETS);

    if (bucket < LAT_SUB_BUCKETS) {   /* las primeras 8 son exactas */
        return bucket;
    }

    uint32_t group = bucket / LAT_SUB_BUCKETS;   /* = e - 2 */
    uint32_t sub = bucket % LAT_SUB_BUCKETS;
    uint32_t e = group + 2u;

    uint64_t width = 1ull << (e - LAT_SUB_BITS);     /* 2^(e-3) */
    uint64_t lo = (1ull << e) + (uint64_t)sub * width;
    return lo + width / 2;   /* punto medio del rango; width siempre par o 1 */
}

void lat_hist_add(struct latency_hist *h, uint64_t ns) {
    h->count++;
    h->sum_ns += ns;
    if (ns > h->max_ns) {
        h->max_ns = ns;
    }
    h->buckets[lat_hist_bucket(ns)]++;
}

double lat_hist_mean_ns(const struct latency_hist *h) {
    if (h->count == 0) {
        return 0.0;
    }
    return (double)h->sum_ns / (double)h->count;
}

uint64_t lat_hist_percentile_ns(const struct latency_hist *h, double p) {
    assert(p > 0.0 && p <= 100.0);

    if (h->count == 0) {
        return 0;
    }

    uint64_t target = (uint64_t)ceil((p / 100.0) * (double)h->count);
    if (target < 1) {
        target = 1;
    }
    if (target > h->count) {
        target = h->count;
    }

    uint64_t cumulative = 0;
    for (uint32_t b = 0; b < LAT_BUCKETS; b++) {
        cumulative += h->buckets[b];
        if (cumulative >= target) {
            return lat_hist_bucket_mid(b);
        }
    }

    /* Inalcanzable si h->count > 0 y las cubetas suman h->count. */
    return h->max_ns;
}

double stats_mean(const double *values, size_t n) {
    assert(n == 0 || values != NULL);

    if (n == 0) {
        return 0.0;
    }

    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        sum += values[i];
    }
    return sum / (double)n;
}

double stats_stddev(const double *values, size_t n) {
    assert(n == 0 || values != NULL);

    if (n < 2) {
        return 0.0;
    }

    double mean = stats_mean(values, n);
    double sum_sq_diff = 0.0;
    for (size_t i = 0; i < n; i++) {
        double diff = values[i] - mean;
        sum_sq_diff += diff * diff;
    }
    return sqrt(sum_sq_diff / (double)(n - 1));   /* muestral: n-1, no n */
}

double throughput_msgs_per_s(uint64_t extracted, uint64_t first_start_ns,
                             uint64_t last_dequeue_ns) {
    if (last_dequeue_ns <= first_start_ns) {
        return 0.0;   /* intervalo invalido o nulo: sin esto, la resta de
                        * enteros sin signo "envolveria" a un numero enorme */
    }

    uint64_t interval_ns = last_dequeue_ns - first_start_ns;
    return (double)extracted * 1e9 / (double)interval_ns;
}
