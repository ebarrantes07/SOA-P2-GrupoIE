/* sync.c — capa de sincronización (D3, D18).
 *
 * Unica capa que toca el mutex y las condvars. put/get siguen al pie de la
 * letra el pseudocodigo de D3, y ademas mantienen los agregados de la
 * instancia que D11 define (rechazos, ultima extraccion, histograma de
 * latencia), porque son los unicos puntos del sistema que los conocen.
 */
#include "sync.h"

#include "buffer.h"
#include "clock.h"
#include "metrics.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    pthread_cond_t *conds[] = { &inst->not_full, &inst->not_empty,
                                &inst->closing, &inst->all_left };
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
    pthread_cond_t *conds[] = { &inst->not_full, &inst->not_empty,
                                &inst->closing, &inst->all_left };

    for (size_t i = 0; i < sizeof(conds) / sizeof(conds[0]); i++)
        if ((rc = pthread_cond_destroy(conds[i])) != 0 && first == 0)
            first = rc;
    if ((rc = pthread_mutex_destroy(&inst->mutex)) != 0 && first == 0)
        first = rc;
    return -first;
}

/* ---- Politica de errores de las primitivas (D18) ---------------------- */

/* Un fallo de lock/unlock/wait/signal/broadcast no es una condicion que
 * el programa pueda manejar: significa que el protocolo ya esta roto
 * (mutex sin inicializar, unlock de un mutex ajeno, deadlock detectado).
 * Ademas estas operaciones devuelven void, asi que no hay por donde
 * reportar. Se aborta con el nombre de la llamada y la causa.
 *
 * Chequeo explicito y no assert: con -DNDEBUG el assert desaparece y el
 * fallo pasaria en silencio, igual que en now_monotonic_ns (#34).
 *
 * A diferencia de clock_gettime, las funciones pthread_* NO usan errno:
 * devuelven el codigo de error directamente, de ahi strerror(rc) y no
 * perror(). */
static void primitive_failed(const char *call, int rc) {
    fprintf(stderr, "sync: %s: %s\n", call, strerror(rc));
    abort();
}

/* ---- Exclusion mutua -------------------------------------------------- */

void sync_lock(struct instance *inst) {
    int rc = pthread_mutex_lock(&inst->mutex);
    if (rc != 0) {
        primitive_failed("pthread_mutex_lock", rc);
    }
}

void sync_unlock(struct instance *inst) {
    int rc = pthread_mutex_unlock(&inst->mutex);
    if (rc != 0) {
        primitive_failed("pthread_mutex_unlock", rc);
    }
}

void sync_broadcast_all(struct instance *inst) {
    /* Las cuatro: pasar a CLOSING vuelve falso el predicado de todos los
     * que esperan, sea espacio, datos, la pausa o el retiro (D3). */
    pthread_cond_t *conds[] = { &inst->not_full, &inst->not_empty,
                                &inst->closing, &inst->all_left };

    for (size_t i = 0; i < sizeof(conds) / sizeof(conds[0]); i++) {
        int rc = pthread_cond_broadcast(conds[i]);
        if (rc != 0) {
            primitive_failed("pthread_cond_broadcast", rc);
        }
    }
}

/* Esperar y senalar siguen la misma politica que lock/unlock (D18): no
 * tienen por donde devolver un error y un fallo aca es protocolo roto. */
static void cond_wait_checked(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    int rc = pthread_cond_wait(cond, mutex);
    if (rc != 0) {
        primitive_failed("pthread_cond_wait", rc);
    }
}

static void cond_signal_checked(pthread_cond_t *cond) {
    int rc = pthread_cond_signal(cond);
    if (rc != 0) {
        primitive_failed("pthread_cond_signal", rc);
    }
}

/* ---- Operaciones bloqueantes (D3) ------------------------------------- */

enum put_result sync_put(struct instance *inst, struct message *msg,
                         struct op_snapshot *snap) {
    sync_lock(inst);

    /* Ciclo while y no if: cond_wait puede despertar de forma espuria, y
     * ademas otro productor puede haber vuelto a llenar el buffer entre
     * el signal y nuestra readquisicion del mutex. */
    while (buffer_is_full(inst) && inst->state == STATE_OPEN) {
        cond_wait_checked(&inst->not_full, &inst->mutex);
    }

    /* El corte le gana al espacio: el productor sale apenas el estado deja
     * de ser OPEN, aunque haya lugar libre (D3). */
    if (inst->state != STATE_OPEN) {
        inst->metrics.rejected_puts++;   /* no cuenta como aceptado (D11) */
        sync_unlock(inst);
        return PUT_REJECTED_CLOSING;
    }

    msg->enqueue_seq = ++inst->enqueue_seq;          /* el primero es 1 */
    msg->enqueue_time_ns = now_monotonic_ns();
    buffer_push(inst, msg);

    if (snap != NULL) {
        snap->seq = msg->enqueue_seq;
        snap->time_ns = msg->enqueue_time_ns;
        snap->occupancy = inst->count;
    }

    /* signal y no broadcast: un push satisface exactamente a un
     * consumidor, y en not_empty solo duermen consumidores (D3). */
    cond_signal_checked(&inst->not_empty);
    sync_unlock(inst);
    return PUT_OK;
}

enum get_result sync_get(struct instance *inst, struct message *out,
                         struct op_snapshot *snap) {
    sync_lock(inst);

    while (buffer_is_empty(inst) && inst->state == STATE_OPEN) {
        cond_wait_checked(&inst->not_empty, &inst->mutex);
    }

    /* Vacio al salir del ciclo implica que el estado ya no es OPEN: si lo
     * fuera y estuviera vacio, seguiriamos dormidos. Por eso alcanza con
     * mirar el buffer (D3). */
    if (buffer_is_empty(inst)) {
        sync_unlock(inst);
        return GET_DRAINED;
    }

    uint64_t dseq = ++inst->dequeue_seq;
    buffer_pop(inst, out);

    /* La marca se toma siempre, no solo cuando hay snapshot: los
     * agregados de D11 la necesitan igual. */
    uint64_t now = now_monotonic_ns();
    inst->metrics.last_dequeue_ns = now;
    lat_hist_add(&inst->metrics.latency, now - out->enqueue_time_ns);

    if (snap != NULL) {
        snap->seq = dseq;
        snap->time_ns = now;
        snap->occupancy = inst->count;
    }

    cond_signal_checked(&inst->not_full);
    sync_unlock(inst);
    return GET_OK;
}
