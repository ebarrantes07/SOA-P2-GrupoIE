/* sync.c — capa de sincronización (D3).
 *
 * Por ahora solo el ciclo de vida de las primitivas, que el creator necesita
 * en el paso 6 de D1 (Issue #9). put/get, lock/unlock y broadcast los agrega
 * el Issue #11.
 */
#include "sync.h"

#include <errno.h>
#include <time.h>
#include <unistd.h>

/* macOS no implementa pthread_condattr_setclock (opción Clock Selection de
 * POSIX). En GNU/Linux, el entorno de referencia, siempre se configura
 * CLOCK_MONOTONIC; en macOS las condvars quedan con el reloj por defecto y
 * solo sirven para desarrollo local. Ver D13. */
#if defined(_POSIX_CLOCK_SELECTION) && _POSIX_CLOCK_SELECTION > 0
#define SYNC_HAS_CONDATTR_SETCLOCK 1
#else
#define SYNC_HAS_CONDATTR_SETCLOCK 0
#endif

int sync_init_primitives(struct instance *inst) {
    pthread_mutexattr_t ma;
    pthread_condattr_t ca;
    int rc;

    if ((rc = pthread_mutexattr_init(&ma)) != 0)
        return -rc;
    if ((rc = pthread_condattr_init(&ca)) != 0) {
        pthread_mutexattr_destroy(&ma);
        return -rc;
    }

    pthread_cond_t *conds[] = { &inst->not_full, &inst->not_empty, &inst->all_left };
    size_t nconds = sizeof(conds) / sizeof(conds[0]);
    size_t made = 0;            /* condvars ya inicializadas, para deshacer */
    bool mutex_made = false;

    if ((rc = pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED)) != 0)
        goto out;
    if ((rc = pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED)) != 0)
        goto out;
#if SYNC_HAS_CONDATTR_SETCLOCK
    if ((rc = pthread_condattr_setclock(&ca, CLOCK_MONOTONIC)) != 0)
        goto out;
#endif

    if ((rc = pthread_mutex_init(&inst->mutex, &ma)) != 0)
        goto out;
    mutex_made = true;

    for (; made < nconds; made++)
        if ((rc = pthread_cond_init(conds[made], &ca)) != 0)
            goto out;

out:
    if (rc != 0) {
        while (made > 0)
            pthread_cond_destroy(conds[--made]);
        if (mutex_made)
            pthread_mutex_destroy(&inst->mutex);
    }
    pthread_condattr_destroy(&ca);
    pthread_mutexattr_destroy(&ma);
    return -rc;
}

int sync_destroy_primitives(struct instance *inst) {
    int first = 0, rc;
    pthread_cond_t *conds[] = { &inst->not_full, &inst->not_empty, &inst->all_left };

    for (size_t i = 0; i < sizeof(conds) / sizeof(conds[0]); i++)
        if ((rc = pthread_cond_destroy(conds[i])) != 0 && first == 0)
            first = rc;
    if ((rc = pthread_mutex_destroy(&inst->mutex)) != 0 && first == 0)
        first = rc;
    return -first;
}
