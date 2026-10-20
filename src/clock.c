#include "clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

uint64_t now_monotonic_ns(void) {
    struct timespec ts;

    /* Chequeo explicito y no un assert, a diferencia de las precondiciones
     * de buffer.c o metrics.c: esas son errores de quien programa y esta
     * bien que -DNDEBUG las compile afuera, pero esto es una llamada al
     * sistema que puede fallar en ejecucion. Sin el chequeo, en modo
     * release ts quedaria sin inicializar y esa basura viajaria a
     * enqueue_time_ns y de ahi a las latencias de D11: mediciones
     * corruptas sin un solo aviso. Abortar sigue la politica de errores de
     * primitivas acordada en #11. */
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime(CLOCK_MONOTONIC)");
        abort();
    }

    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
