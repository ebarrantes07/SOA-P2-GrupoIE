/* test_sync.c — pruebas de la capa de sincronizacion (D3, D18, Issue #11).
 *
 * Este slice cubre el ciclo de vida de las primitivas y la exclusion
 * mutua; put/get llegan en el siguiente. Las pruebas usan procesos reales
 * (fork), que es lo unico que verifica de verdad PTHREAD_PROCESS_SHARED:
 * un mutex mal configurado funciona perfecto entre hilos y falla entre
 * procesos.
 */
#include "sync.h"
#include "buffer.h"
#include "memory.h"
#include "test_util.h"

#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *name_for(const char *tag) {
    static char buf[INSTANCE_NAME_MAX + 1];
    snprintf(buf, sizeof(buf), "/t11_%d_%s", (int)getpid(), tag);
    memory_unlink(buf);           /* restos de una corrida anterior abortada */
    return buf;
}

/* Banderas del propio test, en su memoria compartida: no se usan campos
 * de struct instance para no darles un significado que no tienen. */
struct flags {
    int child_waiting;
    int go;
};

static struct flags *flags_create(void) {
    void *p = mmap(NULL, sizeof(struct flags), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return NULL;
    }
    struct flags *f = p;
    f->child_waiting = 0;
    f->go = 0;
    return f;
}

/* sync_broadcast_all recorre las cuatro condvars sin fallar, con el mutex
 * tomado como exige su precondicion.
 *
 * Ojo con lo que esta prueba NO demuestra: una pthread_cond_t en ceros es
 * un PTHREAD_COND_INITIALIZER valido, asi que broadcast tiene exito aunque
 * la condvar nunca haya pasado por pthread_cond_init -- y por lo tanto no
 * tenga PTHREAD_PROCESS_SHARED. Lo comprobe quitando `closing` del arreglo
 * de init: esta prueba sigue pasando. Lo que de verdad cubre esa falla es
 * broadcast_wakes_waiter_on_closing, que cruza procesos. */
static bool test_broadcast_all_succeeds(void) {
    const char *name = name_for("bcall");
    struct instance *inst;
    size_t len;
    CHECK(memory_create(name, 4, &inst, &len) == MEM_OK);

    sync_lock(inst);
    sync_broadcast_all(inst);
    sync_unlock(inst);

    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* sync_lock/sync_unlock coordinan procesos distintos, no solo hilos. Sin
 * exclusion real, los incrementos se pierden y el total no cuadra. */
static bool test_lock_excludes_across_processes(void) {
    enum { CHILDREN = 4, ROUNDS = 20000 };
    const char *name = name_for("lock");
    struct instance *inst;
    size_t len;
    CHECK(memory_create(name, 1, &inst, &len) == MEM_OK);
    memory_unmap(inst, len);

    for (int c = 0; c < CHILDREN; c++) {
        pid_t pid = fork();
        CHECK(pid != -1);
        if (pid == 0) {
            struct instance *mine;
            size_t mlen;
            if (memory_open(name, &mine, &mlen) != MEM_OK) {
                _exit(2);
            }
            for (int i = 0; i < ROUNDS; i++) {
                sync_lock(mine);
                mine->enqueue_seq++;
                sync_unlock(mine);
            }
            memory_unmap(mine, mlen);
            _exit(0);
        }
    }

    bool children_ok = true;
    for (int c = 0; c < CHILDREN; c++) {
        int status;
        if (wait(&status) == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            children_ok = false;
        }
    }
    CHECK(children_ok);

    CHECK(memory_open(name, &inst, &len) == MEM_OK);
    CHECK(inst->enqueue_seq == (uint64_t)CHILDREN * ROUNDS);
    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* La condvar nueva (`closing`, para la pausa de #13) despierta entre
 * procesos: el hijo duerme en ella y el padre lo saca con
 * sync_broadcast_all. Es lo que no cubria ninguna prueba previa. */
static bool test_broadcast_wakes_waiter_on_closing(void) {
    const char *name = name_for("bcast");
    struct instance *inst;
    size_t len;
    CHECK(memory_create(name, 1, &inst, &len) == MEM_OK);

    struct flags *f = flags_create();
    CHECK(f != NULL);

    pid_t pid = fork();
    CHECK(pid != -1);
    if (pid == 0) {
        alarm(10);              /* si nunca despierta, muere y la prueba falla */
        struct instance *mine;
        size_t mlen;
        if (memory_open(name, &mine, &mlen) != MEM_OK) {
            _exit(2);
        }
        sync_lock(mine);
        f->child_waiting = 1;
        /* El flag se publica con el mutex tomado y cond_wait lo suelta de
         * forma atomica, asi que el padre no puede colarse entre ambos y
         * perder el despertar. */
        while (!f->go) {
            if (pthread_cond_wait(&mine->closing, &mine->mutex) != 0) {
                _exit(3);
            }
        }
        sync_unlock(mine);
        memory_unmap(mine, mlen);
        _exit(0);
    }

    /* Esperar a que el hijo este dormido de verdad antes de despertarlo. */
    for (;;) {
        sync_lock(inst);
        int waiting = f->child_waiting;
        sync_unlock(inst);
        if (waiting) {
            break;
        }
        /* usleep salio de POSIX.1-2008; nanosleep es la portable. */
        struct timespec ms = {0, 1000000};
        nanosleep(&ms, NULL);
    }

    sync_lock(inst);
    f->go = 1;
    sync_broadcast_all(inst);
    sync_unlock(inst);

    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);

    munmap(f, sizeof(*f));
    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* ---- put / get (D3) --------------------------------------------------- */

/* Instancia lista para usar, con limpieza a cargo de quien la pide. */
static struct instance *open_instance(const char *tag, uint32_t capacity,
                                      const char **out_name, size_t *out_len) {
    const char *name = name_for(tag);
    struct instance *inst;
    if (memory_create(name, capacity, &inst, out_len) != MEM_OK) {
        return NULL;
    }
    *out_name = name;
    return inst;
}

static struct message msg_with(uint32_t producer_id, uint64_t sequence) {
    struct message m = {0};
    m.producer_id = producer_id;
    m.sequence = sequence;
    return m;
}

/* put asigna enqueue_seq desde 1 y una marca bajo el mutex; el snapshot
 * refleja la ocupacion justo despues de insertar. */
static bool test_put_assigns_seq_time_and_snapshot(void) {
    const char *name;
    size_t len;
    struct instance *inst = open_instance("put", 4, &name, &len);
    CHECK(inst != NULL);

    uint64_t previa = 0;
    for (uint64_t i = 1; i <= 3; i++) {
        struct message m = msg_with(1, i);
        struct op_snapshot s = {0};
        CHECK(sync_put(inst, &m, &s) == PUT_OK);
        CHECK(m.enqueue_seq == i);               /* el primero es 1 */
        CHECK(m.enqueue_time_ns > previa);       /* reloj monotonico */
        CHECK(s.seq == i);
        CHECK(s.time_ns == m.enqueue_time_ns);
        CHECK(s.occupancy == (uint32_t)i);       /* count despues del push */
        previa = m.enqueue_time_ns;
    }
    CHECK(inst->count == 3);

    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* get devuelve en orden FIFO, numera las extracciones desde 1 y deja la
 * ocupacion que corresponde. */
static bool test_get_is_fifo_and_numbers_extractions(void) {
    const char *name;
    size_t len;
    struct instance *inst = open_instance("get", 4, &name, &len);
    CHECK(inst != NULL);

    for (uint64_t i = 1; i <= 3; i++) {
        struct message m = msg_with(7, i);
        CHECK(sync_put(inst, &m, NULL) == PUT_OK);   /* snapshot opcional */
    }

    for (uint64_t i = 1; i <= 3; i++) {
        struct message out;
        struct op_snapshot s = {0};
        CHECK(sync_get(inst, &out, &s) == GET_OK);
        CHECK(out.producer_id == 7 && out.sequence == i);   /* FIFO */
        CHECK(out.enqueue_seq == i);
        CHECK(s.seq == i);                                  /* dequeue_seq */
        CHECK(s.occupancy == (uint32_t)(3 - i));
    }
    CHECK(buffer_is_empty(inst));

    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* En CLOSING el productor se va rechazado aunque sobre espacio, y el
 * rechazo queda contado aparte: no es un mensaje aceptado (D11). */
static bool test_put_rejected_when_closing(void) {
    const char *name;
    size_t len;
    struct instance *inst = open_instance("rej", 4, &name, &len);
    CHECK(inst != NULL);

    sync_lock(inst);
    inst->state = STATE_CLOSING;
    sync_unlock(inst);

    struct message m = msg_with(1, 1);
    struct op_snapshot s = {0};
    CHECK(sync_put(inst, &m, &s) == PUT_REJECTED_CLOSING);
    CHECK(inst->count == 0);                     /* no se inserto nada */
    CHECK(inst->enqueue_seq == 0);               /* no consumio un numero */
    CHECK(inst->metrics.rejected_puts == 1);

    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* El consumidor sigue drenando en CLOSING mientras queden mensajes, y
 * recien avisa cuando no pueden llegar mas. */
static bool test_get_drains_before_reporting_drained(void) {
    const char *name;
    size_t len;
    struct instance *inst = open_instance("drain", 4, &name, &len);
    CHECK(inst != NULL);

    for (uint64_t i = 1; i <= 2; i++) {
        struct message m = msg_with(1, i);
        CHECK(sync_put(inst, &m, NULL) == PUT_OK);
    }

    sync_lock(inst);
    inst->state = STATE_CLOSING;
    sync_unlock(inst);

    struct message out;
    CHECK(sync_get(inst, &out, NULL) == GET_OK && out.sequence == 1);
    CHECK(sync_get(inst, &out, NULL) == GET_OK && out.sequence == 2);
    CHECK(sync_get(inst, &out, NULL) == GET_DRAINED);

    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* Los agregados de D11 que solo sync_get puede mantener. */
static bool test_get_feeds_instance_metrics(void) {
    const char *name;
    size_t len;
    struct instance *inst = open_instance("metr", 4, &name, &len);
    CHECK(inst != NULL);

    CHECK(inst->metrics.latency.count == 0);
    CHECK(inst->metrics.last_dequeue_ns == 0);

    for (uint64_t i = 1; i <= 3; i++) {
        struct message m = msg_with(1, i);
        CHECK(sync_put(inst, &m, NULL) == PUT_OK);
    }
    for (int i = 0; i < 3; i++) {
        struct message out;
        CHECK(sync_get(inst, &out, NULL) == GET_OK);
    }

    CHECK(inst->metrics.latency.count == 3);     /* una muestra por extraccion */
    CHECK(inst->metrics.last_dequeue_ns > 0);
    CHECK(inst->metrics.latency.sum_ns > 0);     /* latencia real, no cero */

    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

int main(void) {
    struct test_case tests[] = {
        {"broadcast_all_succeeds", test_broadcast_all_succeeds},
        {"lock_excludes_across_processes", test_lock_excludes_across_processes},
        {"broadcast_wakes_waiter_on_closing", test_broadcast_wakes_waiter_on_closing},
        {"put_assigns_seq_time_and_snapshot", test_put_assigns_seq_time_and_snapshot},
        {"get_is_fifo_and_numbers_extractions", test_get_is_fifo_and_numbers_extractions},
        {"put_rejected_when_closing", test_put_rejected_when_closing},
        {"get_drains_before_reporting_drained", test_get_drains_before_reporting_drained},
        {"get_feeds_instance_metrics", test_get_feeds_instance_metrics},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
