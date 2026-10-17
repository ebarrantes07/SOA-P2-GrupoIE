/* test_clock.c — prueba de now_monotonic_ns() (Issue #5). */
#include "clock.h"
#include "test_util.h"

static bool test_never_decreases(void) {
    uint64_t prev = now_monotonic_ns();
    for (int i = 0; i < 10000; i++) {
        uint64_t now = now_monotonic_ns();
        CHECK(now >= prev);   /* unica garantia real de un reloj monotonico */
        prev = now;
    }
    return true;
}

int main(void) {
    struct test_case tests[] = {
        {"never_decreases", test_never_decreases},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
