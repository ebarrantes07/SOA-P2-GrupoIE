#include "clock.h"

#include <assert.h>
#include <time.h>

uint64_t now_monotonic_ns(void) {
    struct timespec ts;
    int rc = clock_gettime(CLOCK_MONOTONIC, &ts);
    assert(rc == 0);   /* CLOCK_MONOTONIC con un puntero valido no deberia fallar */

    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
