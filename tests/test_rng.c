/* test_rng.c — pruebas del RNG xorshift32 y la pausa exponencial
 * (Issue #5). Comparaciones exactas; sin estadistica (la convergencia de
 * la media se verifico a mano, ver el PR).
 */
#include "rng.h"
#include "test_util.h"

#include <math.h>

static bool test_seed_zero_rejected(void) {
    struct rng r;
    r.state = 0xDEADBEEFu;   /* centinela: si rng_init tocara el estado, se notaria */
    CHECK(rng_init(&r, 0) == -1);
    CHECK(r.state == 0xDEADBEEFu);   /* "rng no se modifica" */
    return true;
}

static bool test_same_seed_reproduces_sequence(void) {
    struct rng a, b;
    rng_init(&a, 12345);
    rng_init(&b, 12345);
    for (int i = 0; i < 1000; i++) {
        CHECK(rng_next(&a) == rng_next(&b));
    }
    return true;
}

static bool test_uniform01_never_zero(void) {
    struct rng r;
    rng_init(&r, 999);
    for (int i = 0; i < 1000000; i++) {
        double u = rng_uniform01(&r);
        CHECK(u > 0.0);
        CHECK(u <= 1.0);
    }
    return true;
}

static bool test_exponential_matches_formula(void) {
    /* Dos generadores con la misma semilla producen el mismo u; comparar
     * rng_exponential_ms contra -mean*ln(u) calculado aparte agarra un
     * error de formula (signo, log10 en vez de log, orden) sin apoyarse
     * en estadistica. */
    struct rng a, b;
    rng_init(&a, 42);
    rng_init(&b, 42);

    double mean_ms = 7.5;
    double x = rng_exponential_ms(&a, mean_ms);
    double u = rng_uniform01(&b);
    double expected = -mean_ms * log(u);

    CHECK(x == expected);
    return true;
}

int main(void) {
    struct test_case tests[] = {
        {"seed_zero_rejected", test_seed_zero_rejected},
        {"same_seed_reproduces_sequence", test_same_seed_reproduces_sequence},
        {"uniform01_never_zero", test_uniform01_never_zero},
        {"exponential_matches_formula", test_exponential_matches_formula},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
