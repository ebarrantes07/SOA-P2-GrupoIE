/* test_metrics.c — pruebas del histograma de latencia y la estadística
 * (Issue #8). Datos de entrada conocidos, resultado calculado a mano;
 * sin estadística con tolerancia.
 */
#include "metrics.h"
#include "test_util.h"

static bool test_bucket_boundaries(void) {
    /* Derivados a mano (ver docs/decisiones_diseno.md, D11). */
    CHECK(lat_hist_bucket(7) == 7);     /* ultima cubeta exacta */
    CHECK(lat_hist_bucket(8) == 8);     /* primer valor del grupo e=3 */
    CHECK(lat_hist_bucket(15) == 15);   /* ultimo del grupo e=3 */
    CHECK(lat_hist_bucket(16) == 16);   /* primer valor del grupo e=4, ancho 2 */
    CHECK(lat_hist_bucket(17) == 16);   /* mismo sub-cajon que 16 */
    CHECK(lat_hist_bucket(31) == 23);   /* ultimo del grupo e=4 */
    CHECK(lat_hist_bucket(UINT64_MAX) == LAT_BUCKETS - 1);
    return true;
}

static bool test_bucket_mid_error_bound(void) {
    /* Para cada ns, el punto medio de su cubeta no se aleja mas del
     * 6.25% documentado. Muestras fijas, no aleatorias. */
    static const uint64_t samples[] = {100, 1000, 1000000, 5000000000ull};
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        uint64_t ns = samples[i];
        uint64_t mid = lat_hist_bucket_mid(lat_hist_bucket(ns));
        uint64_t diff = mid > ns ? mid - ns : ns - mid;
        /* diff / ns <= 0.0625  <=>  diff * 16 <= ns * 1  (evita division) */
        CHECK(diff * 16 <= ns);
    }
    return true;
}

static bool test_hist_add_and_mean(void) {
    struct latency_hist h = {0};
    lat_hist_add(&h, 2);
    lat_hist_add(&h, 4);
    lat_hist_add(&h, 6);
    CHECK(h.count == 3);
    CHECK(h.sum_ns == 12);
    CHECK(h.max_ns == 6);
    CHECK(lat_hist_mean_ns(&h) == 4.0);   /* (2+4+6)/3 */
    return true;
}

static bool test_percentile_known_values(void) {
    struct latency_hist h = {0};
    for (uint64_t v = 1; v <= 7; v++) {
        lat_hist_add(&h, v);   /* 1..7, todas en cubetas exactas */
    }
    CHECK(lat_hist_percentile_ns(&h, 50.0) == 4);    /* ceil(0.5*7)=4to valor */
    CHECK(lat_hist_percentile_ns(&h, 100.0) == 7);
    return true;
}

static bool test_percentile_empty(void) {
    struct latency_hist h = {0};
    CHECK(lat_hist_percentile_ns(&h, 95.0) == 0);
    CHECK(lat_hist_mean_ns(&h) == 0.0);
    return true;
}

static bool test_stats_mean_stddev(void) {
    double vals[] = {2.0, 4.0, 6.0};
    /* a mano: mean=4; varianza muestral = ((2-4)^2+(4-4)^2+(6-4)^2)/(3-1) = 4; sd=2 */
    CHECK(stats_mean(vals, 3) == 4.0);
    CHECK(stats_stddev(vals, 3) == 2.0);
    return true;
}

static bool test_stats_edge_cases(void) {
    double one[] = {5.0};
    CHECK(stats_stddev(one, 1) == 0.0);
    CHECK(stats_mean(NULL, 0) == 0.0);
    CHECK(stats_stddev(NULL, 0) == 0.0);
    return true;
}

static bool test_throughput(void) {
    /* 100 mensajes en 1 segundo exacto */
    CHECK(throughput_msgs_per_s(100, 0, 1000000000ull) == 100.0);
    /* intervalo nulo */
    CHECK(throughput_msgs_per_s(50, 1000000000ull, 1000000000ull) == 0.0);
    /* orden invertido: NO debe envolver a un numero gigante */
    CHECK(throughput_msgs_per_s(50, 2000000000ull, 1000000000ull) == 0.0);
    return true;
}

int main(void) {
    struct test_case tests[] = {
        {"bucket_boundaries", test_bucket_boundaries},
        {"bucket_mid_error_bound", test_bucket_mid_error_bound},
        {"hist_add_and_mean", test_hist_add_and_mean},
        {"percentile_known_values", test_percentile_known_values},
        {"percentile_empty", test_percentile_empty},
        {"stats_mean_stddev", test_stats_mean_stddev},
        {"stats_edge_cases", test_stats_edge_cases},
        {"throughput", test_throughput},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
