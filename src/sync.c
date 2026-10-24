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

/* pthread_cond_timedwait no recibe una duracion sino un instante
 * absoluto, y lo compara contra el reloj con el que se configuro la
 * condvar. Asi que el plazo hay que leerlo de ESE reloj, no del "reloj
 * del proyecto": si la condvar quedo en CLOCK_REALTIME (macOS, donde el
 * #if de arriba es 0) y el plazo se calcula con CLOCK_MONOTONIC, se le
 * pasa un instante de los anos 70 y timedwait devuelve ETIMEDOUT de
 * inmediato -- la pausa desaparece sin un solo error, porque ETIMEDOUT es
 * justo el codigo del fin normal. Medido: 0 ms en vez de 300.
 *
 * Las dos decisiones cuelgan del mismo macro a proposito, para que no
 * puedan desalinearse. Ver D20. */
#if SYNC_HAS_CONDATTR_SETCLOCK
#define SYNC_COND_CLOCK CLOCK_MONOTONIC
#else
#define SYNC_COND_CLOCK CLOCK_REALTIME   /* el que usa pthread_cond_init por defecto */
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

void sync_signal_all_left(struct instance *inst) {
    cond_signal_checked(&inst->all_left);
}

/* ---- Medicion de esperas para op_snapshot (D11) ----------------------- */

/* El reloj solo se lee si el llamador pidio snapshot: quien no quiere las
 * metricas no paga las lecturas. Medir lock_ns cuesta dos lecturas por
 * operacion aunque no haya contencion, que es el precio de los datos que
 * D11 pide para el resumen por proceso. */
static uint64_t mark_if(bool on) {
    return on ? now_monotonic_ns() : 0;
}

static uint64_t since_if(bool on, uint64_t mark) {
    return on ? now_monotonic_ns() - mark : 0;
}

/* ---- Operaciones bloqueantes (D3) ------------------------------------- */

enum put_result sync_put(struct instance *inst, struct message *msg,
                         struct op_snapshot *snap) {
    const bool measure = (snap != NULL);
    if (measure) {
        *snap = (struct op_snapshot){0};   /* todo campo queda definido */
    }

    uint64_t mark = mark_if(measure);
    sync_lock(inst);
    uint64_t lock_ns = since_if(measure, mark);
    uint64_t wait_ns = 0;
    uint32_t wait_count = 0;

    /* Ciclo while y no if: cond_wait puede despertar de forma espuria, y
     * ademas otro productor puede haber vuelto a llenar el buffer entre
     * el signal y nuestra readquisicion del mutex. */
    while (buffer_is_full(inst) && inst->state == STATE_OPEN) {
        uint64_t slept = mark_if(measure);
        cond_wait_checked(&inst->not_full, &inst->mutex);
        if (measure) {
            wait_ns += since_if(true, slept);
            wait_count++;
        }
    }

    /* El corte le gana al espacio: el productor sale apenas el estado deja
     * de ser OPEN, aunque haya lugar libre (D3). */
    if (inst->state != STATE_OPEN) {
        inst->metrics.rejected_puts++;   /* no cuenta como aceptado (D11) */
        if (measure) {
            /* La espera ocurrio aunque el intento terminara rechazado: no
             * se descarta, o el resumen por proceso la perderia. */
            snap->lock_ns = lock_ns;
            snap->wait_ns = wait_ns;
            snap->wait_count = wait_count;
        }
        sync_unlock(inst);
        return PUT_REJECTED_CLOSING;
    }

    msg->enqueue_seq = ++inst->enqueue_seq;          /* el primero es 1 */
    msg->enqueue_time_ns = now_monotonic_ns();
    buffer_push(inst, msg);

    if (measure) {
        snap->seq = msg->enqueue_seq;
        snap->time_ns = msg->enqueue_time_ns;
        snap->occupancy = inst->count;
        snap->lock_ns = lock_ns;
        snap->wait_ns = wait_ns;
        snap->wait_count = wait_count;
    }

    /* signal y no broadcast: un push satisface exactamente a un
     * consumidor, y en not_empty solo duermen consumidores (D3). */
    cond_signal_checked(&inst->not_empty);
    sync_unlock(inst);
    return PUT_OK;
}

enum get_result sync_get(struct instance *inst, struct message *out,
                         struct op_snapshot *snap) {
    const bool measure = (snap != NULL);
    if (measure) {
        *snap = (struct op_snapshot){0};
    }

    uint64_t mark = mark_if(measure);
    sync_lock(inst);
    uint64_t lock_ns = since_if(measure, mark);
    uint64_t wait_ns = 0;
    uint32_t wait_count = 0;

    while (buffer_is_empty(inst) && inst->state == STATE_OPEN) {
        uint64_t slept = mark_if(measure);
        cond_wait_checked(&inst->not_empty, &inst->mutex);
        if (measure) {
            wait_ns += since_if(true, slept);
            wait_count++;
        }
    }

    /* Vacio al salir del ciclo implica que el estado ya no es OPEN: si lo
     * fuera y estuviera vacio, seguiriamos dormidos. Por eso alcanza con
     * mirar el buffer (D3). */
    if (buffer_is_empty(inst)) {
        if (measure) {
            snap->lock_ns = lock_ns;
            snap->wait_ns = wait_ns;
            snap->wait_count = wait_count;
        }
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

    if (measure) {
        snap->seq = dseq;
        snap->time_ns = now;
        snap->occupancy = inst->count;
        snap->lock_ns = lock_ns;
        snap->wait_ns = wait_ns;
        snap->wait_count = wait_count;
    }

    cond_signal_checked(&inst->not_full);
    sync_unlock(inst);
    return GET_OK;
}

/* ---- Pausa interrumpible por el cierre (#13, D20) --------------------- */

/* Plazo absoluto a `ms` milisegundos de ahora, en el reloj de la condvar.
 * Precondicion: ms > 0 y finito (lo garantiza sync_pause_ms). */
static void deadline_from_now(struct timespec *out, double ms) {
    clock_gettime(SYNC_COND_CLOCK, out);

    uint64_t ns = (uint64_t)(ms * 1000000.0);
    out->tv_sec += (time_t)(ns / 1000000000ull);
    out->tv_nsec += (long)(ns % 1000000000ull);
    if (out->tv_nsec >= 1000000000L) {
        out->tv_sec += 1;
        out->tv_nsec -= 1000000000L;
    }
}

enum pause_result sync_pause_ms(struct instance *inst, double ms) {
    /* El plazo se fija antes de pelear por el mutex: la pausa se cuenta
     * desde la llamada. */
    struct timespec deadline;
    bool wait = (ms > 0.0);   /* falso tambien para NaN */
    if (wait) {
        deadline_from_now(&deadline, ms);
    }

    sync_lock(inst);

    bool expired = false;
    while (wait && !expired && inst->state == STATE_OPEN) {
        int rc = pthread_cond_timedwait(&inst->closing, &inst->mutex, &deadline);
        if (rc == ETIMEDOUT) {
            expired = true;        /* fin normal de la pausa, no un error */
        } else if (rc != 0) {
            primitive_failed("pthread_cond_timedwait", rc);
        }
        /* rc == 0 puede ser un despertar espurio: el while reevalua el
         * estado y el plazo. */
    }

    /* Si el corte llego junto con el vencimiento, se informa interrumpida:
     * el productor no debe insertar, y sync_put lo rechazaria igual. */
    enum pause_result r = (inst->state == STATE_OPEN) ? PAUSE_COMPLETED
                                                      : PAUSE_INTERRUPTED;
    sync_unlock(inst);
    return r;
}
