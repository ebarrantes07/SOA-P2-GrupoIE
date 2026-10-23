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
#include "clock.h"
#include "memory.h"
#include "test_util.h"

#include <stdatomic.h>
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
    int started;
    int done;
    int pause_result;
    unsigned long long pause_ms;
    unsigned long long wait_ns;
    unsigned wait_count;
};

static struct flags *flags_create(void) {
    void *p = mmap(NULL, sizeof(struct flags), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return NULL;
    }
    struct flags *f = p;
    *f = (struct flags){0};
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

/* ---- Medicion de esperas (D11) ---------------------------------------- */

/* Sin contencion no hay esperas que contar. lock_ns se mide igual, y el
 * tope detecta una resta invertida: con unsigned, now - t0 al reves da un
 * numero astronomico en vez de negativo. */
static bool test_snapshot_without_contention(void) {
    const char *name;
    size_t len;
    struct instance *inst = open_instance("nowait", 4, &name, &len);
    CHECK(inst != NULL);

    struct message m = msg_with(1, 1);
    struct op_snapshot s = {0};
    CHECK(sync_put(inst, &m, &s) == PUT_OK);
    CHECK(s.wait_count == 0);
    CHECK(s.wait_ns == 0);
    CHECK(s.lock_ns < 1000000000ull);        /* < 1 s: no hubo resta invertida */

    struct message out;
    struct op_snapshot g = {0};
    CHECK(sync_get(inst, &out, &g) == GET_OK);
    CHECK(g.wait_count == 0 && g.wait_ns == 0);
    CHECK(g.lock_ns < 1000000000ull);

    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* Un productor contra un buffer lleno tiene que dormirse de verdad, y la
 * espera tiene que quedar contada. El padre comprueba primero que el hijo
 * sigue bloqueado y recien despues le hace lugar: eso demuestra que put
 * bloquea, que las pruebas de un solo proceso no podian mostrar. */
static bool test_put_blocks_and_counts_the_wait(void) {
    const char *name;
    size_t len;
    struct instance *inst = open_instance("wait", 1, &name, &len);   /* capacidad 1 */
    CHECK(inst != NULL);

    struct message first = msg_with(1, 1);
    CHECK(sync_put(inst, &first, NULL) == PUT_OK);
    CHECK(buffer_is_full(inst));

    struct flags *f = flags_create();
    CHECK(f != NULL);

    pid_t pid = fork();
    CHECK(pid != -1);
    if (pid == 0) {
        alarm(10);
        struct instance *mine;
        size_t mlen;
        if (memory_open(name, &mine, &mlen) != MEM_OK) {
            _exit(2);
        }
        struct message m = msg_with(2, 1);
        struct op_snapshot s = {0};
        f->started = 1;
        if (sync_put(mine, &m, &s) != PUT_OK) {
            _exit(3);
        }
        f->wait_ns = s.wait_ns;
        f->wait_count = s.wait_count;
        f->done = 1;
        memory_unmap(mine, mlen);
        _exit(0);
    }

    while (!f->started) {
        struct timespec ms = {0, 1000000};
        nanosleep(&ms, NULL);
    }
    struct timespec pausa = {0, 50 * 1000000};   /* 50 ms de margen */
    nanosleep(&pausa, NULL);
    CHECK(f->done == 0);        /* sigue bloqueado: put espera, no falla */

    struct message out;
    CHECK(sync_get(inst, &out, NULL) == GET_OK);   /* le hace lugar */

    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(f->done == 1);
    CHECK(f->wait_count >= 1);                     /* durmio al menos una vez */
    CHECK(f->wait_ns > 0);

    munmap(f, sizeof(*f));
    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* ---- Varios productores y consumidores reales (criterio del #11) ------ */

/* TSan instrumenta hilos dentro de un proceso: no detecta carreras ENTRE
 * procesos sobre memoria compartida, que es justo lo que hay que probar
 * aca. Asi que la verificacion es por invariantes, que ademas son mas
 * fuertes que lo que TSan podria decir: cada identidad extraida
 * exactamente una vez, y el orden global intacto. */

enum { PRODUCERS = 4, CONSUMERS = 4, PER_PRODUCER = 250 };
#define TOTAL (PRODUCERS * PER_PRODUCER)

/* Una casilla por mensaje posible. Los consumidores marcan con fetch_add,
 * asi una doble extraccion se delata como un 2 en vez de perderse en una
 * carrera entre dos procesos. */
struct tally {
    _Atomic unsigned seen[TOTAL];
};

static struct tally *tally_create(void) {
    void *p = mmap(NULL, sizeof(struct tally), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return NULL;
    }
    struct tally *t = p;
    for (size_t i = 0; i < TOTAL; i++) {
        atomic_store(&t->seen[i], 0u);
    }
    return t;
}

static void run_producer(const char *name, uint32_t id) {
    alarm(15);   /* margen 15x sobre el ~1 s de una corrida sana */
    struct instance *inst;
    size_t len;
    if (memory_open(name, &inst, &len) != MEM_OK) {
        _exit(2);
    }
    for (uint64_t i = 1; i <= PER_PRODUCER; i++) {
        struct message m = msg_with(id, i);
        struct op_snapshot s = {0};
        if (sync_put(inst, &m, &s) != PUT_OK) {
            _exit(3);          /* nadie deberia rechazar: aun no hay corte */
        }
        if (s.seq != m.enqueue_seq || s.occupancy == 0) {
            _exit(4);          /* el snapshot no refleja la operacion */
        }
    }
    memory_unmap(inst, len);
    _exit(0);
}

static void run_consumer(const char *name, struct tally *t) {
    alarm(15);   /* margen 15x sobre el ~1 s de una corrida sana */
    struct instance *inst;
    size_t len;
    if (memory_open(name, &inst, &len) != MEM_OK) {
        _exit(2);
    }
    for (;;) {
        struct message out;
        struct op_snapshot s = {0};
        enum get_result r = sync_get(inst, &out, &s);
        if (r == GET_DRAINED) {
            break;
        }
        /* FIFO global: con una sola cola, el extraido k-esimo tiene que
         * ser el insertado k-esimo. */
        if (out.enqueue_seq != s.seq) {
            _exit(5);
        }
        if (out.producer_id < 1 || out.producer_id > PRODUCERS ||
            out.sequence < 1 || out.sequence > PER_PRODUCER) {
            _exit(6);          /* identidad corrupta */
        }
        size_t slot = (size_t)(out.producer_id - 1) * PER_PRODUCER +
                      (size_t)(out.sequence - 1);
        atomic_fetch_add(&t->seen[slot], 1u);
    }
    memory_unmap(inst, len);
    _exit(0);
}

/* Capacidad 4 contra 8 procesos: el buffer se llena y se vacia todo el
 * tiempo, asi que productores y consumidores se bloquean de verdad. */
static bool test_many_processes_conserve_every_message(void) {
    const char *name;
    size_t len;
    struct instance *inst = open_instance("many", 4, &name, &len);
    CHECK(inst != NULL);

    struct tally *t = tally_create();
    CHECK(t != NULL);

    pid_t prod[PRODUCERS], cons[CONSUMERS];
    for (int i = 0; i < CONSUMERS; i++) {
        cons[i] = fork();
        CHECK(cons[i] != -1);
        if (cons[i] == 0) {
            run_consumer(name, t);
        }
    }
    for (int i = 0; i < PRODUCERS; i++) {
        prod[i] = fork();
        CHECK(prod[i] != -1);
        if (prod[i] == 0) {
            run_producer(name, (uint32_t)(i + 1));
        }
    }

    /* Todos los productores terminan antes del corte: si no, sus
     * inserciones se rechazarian y la conservacion fallaria por el
     * guion de la prueba, no por un bug. */
    bool ok = true;
    for (int i = 0; i < PRODUCERS; i++) {
        int st;
        if (waitpid(prod[i], &st, 0) != prod[i] || !WIFEXITED(st) ||
            WEXITSTATUS(st) != 0) {
            ok = false;
        }
    }
    CHECK(ok);

    /* El padre hace de finisher: corta y despierta a todos. */
    sync_lock(inst);
    inst->state = STATE_CLOSING;
    sync_broadcast_all(inst);
    sync_unlock(inst);

    for (int i = 0; i < CONSUMERS; i++) {
        int st;
        if (waitpid(cons[i], &st, 0) != cons[i] || !WIFEXITED(st) ||
            WEXITSTATUS(st) != 0) {
            ok = false;
        }
    }
    CHECK(ok);

    /* Cada mensaje salio exactamente una vez: 0 seria perdida, 2 o mas
     * duplicado. */
    unsigned perdidos = 0, duplicados = 0;
    for (size_t i = 0; i < TOTAL; i++) {
        unsigned v = atomic_load(&t->seen[i]);
        if (v == 0) {
            perdidos++;
        } else if (v > 1) {
            duplicados++;
        }
    }
    CHECK(perdidos == 0);
    CHECK(duplicados == 0);

    CHECK(inst->enqueue_seq == TOTAL);
    CHECK(inst->dequeue_seq == TOTAL);
    CHECK(inst->count == 0);
    CHECK(inst->metrics.latency.count == TOTAL);   /* una muestra por extraccion */
    CHECK(inst->metrics.rejected_puts == 0);

    munmap(t, sizeof(*t));
    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* ---- Pausa interrumpible (#13, D20) ----------------------------------- */

/* La pausa tiene que durar de verdad. Esta prueba es la regresion del
 * problema del reloj: si el plazo se leyera de un reloj distinto al de la
 * condvar, timedwait devolveria ETIMEDOUT al instante y la pausa medirla
 * 0 ms en vez de los 120 pedidos. */
static bool test_pause_actually_waits(void) {
    const char *name;
    size_t len;
    struct instance *inst = open_instance("pause", 4, &name, &len);
    CHECK(inst != NULL);

    /* Si el plazo se leyera de un reloj POSTERIOR al de la condvar, la
     * espera duraria decadas: la alarma convierte el cuelgue en falla. */
    alarm(15);
    uint64_t t0 = now_monotonic_ns();
    CHECK(sync_pause_ms(inst, 120.0) == PAUSE_COMPLETED);
    uint64_t ms = (now_monotonic_ns() - t0) / 1000000ull;
    alarm(0);

    CHECK(ms >= 100);        /* no se evaporo */
    CHECK(ms < 2000);        /* ni se fue de largo */

    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* Con la instancia ya fuera de OPEN no hay nada que esperar. */
static bool test_pause_skips_when_already_closing(void) {
    const char *name;
    size_t len;
    struct instance *inst = open_instance("pskip", 4, &name, &len);
    CHECK(inst != NULL);

    sync_lock(inst);
    inst->state = STATE_CLOSING;
    sync_unlock(inst);

    alarm(15);
    uint64_t t0 = now_monotonic_ns();
    CHECK(sync_pause_ms(inst, 5000.0) == PAUSE_INTERRUPTED);
    CHECK((now_monotonic_ns() - t0) / 1000000ull < 500);   /* no durmio los 5 s */
    alarm(0);

    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* Una duracion no positiva no espera (el caso --mean-ms 0 lo resuelve el
 * llamador, pero la funcion no debe quedar indefinida). */
static bool test_pause_zero_does_not_wait(void) {
    const char *name;
    size_t len;
    struct instance *inst = open_instance("pzero", 4, &name, &len);
    CHECK(inst != NULL);

    alarm(15);
    uint64_t t0 = now_monotonic_ns();
    CHECK(sync_pause_ms(inst, 0.0) == PAUSE_COMPLETED);
    CHECK(sync_pause_ms(inst, -5.0) == PAUSE_COMPLETED);
    CHECK((now_monotonic_ns() - t0) / 1000000ull < 500);
    alarm(0);

    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* El criterio central del #13: un cierre pedido durante la pausa la corta
 * y el productor sale sin insertar. El hijo pide 5 s y debe salir en
 * mucho menos. */
static bool test_pause_interrupted_by_closing(void) {
    const char *name;
    size_t len;
    struct instance *inst = open_instance("pint", 4, &name, &len);
    CHECK(inst != NULL);

    struct flags *f = flags_create();
    CHECK(f != NULL);

    pid_t pid = fork();
    CHECK(pid != -1);
    if (pid == 0) {
        alarm(15);
        struct instance *mine;
        size_t mlen;
        if (memory_open(name, &mine, &mlen) != MEM_OK) {
            _exit(2);
        }
        f->started = 1;
        uint64_t t0 = now_monotonic_ns();
        enum pause_result r = sync_pause_ms(mine, 5000.0);
        f->pause_ms = (now_monotonic_ns() - t0) / 1000000ull;
        f->pause_result = (int)r;
        f->done = 1;
        memory_unmap(mine, mlen);
        _exit(0);
    }

    while (!f->started) {
        struct timespec ms = {0, 1000000};
        nanosleep(&ms, NULL);
    }
    struct timespec settle = {0, 100 * 1000000};   /* que entre a la pausa */
    nanosleep(&settle, NULL);
    CHECK(f->done == 0);                           /* sigue pausado */

    sync_lock(inst);
    inst->state = STATE_CLOSING;
    sync_broadcast_all(inst);
    sync_unlock(inst);

    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(f->done == 1);
    CHECK(f->pause_result == (int)PAUSE_INTERRUPTED);
    CHECK(f->pause_ms < 2000);      /* pidio 5000: salio mucho antes */

    munmap(f, sizeof(*f));
    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* ---- El despertar no se roba (revision de #29) ------------------------ */

/* Escenario: buffer lleno, un productor bloqueado en not_full y varios en
 * pausa larga. Al extraer un mensaje, el bloqueado tiene que insertar sin
 * esperar a que termine la pausa de los otros.
 *
 * Es la prueba que justifica que la pausa tenga condvar propia. Si
 * durmiera en not_full, el unico signal que emite sync_get podria
 * despertar a un productor en pausa, que ve su plazo sin vencer y vuelve
 * a dormirse: el bloqueado por espacio se queda esperando con lugar
 * disponible. Con varios en pausa, la probabilidad de que el signal se
 * desvie es alta y el robo se nota. */

enum { PAUSERS = 4 };

struct steal_flags {
    int paused_started[PAUSERS];
    int paused_done[PAUSERS];
    int blocker_started;
    int blocker_done;
    unsigned long long blocker_ms;
    int blocker_ok;
};

static struct steal_flags *steal_create(void) {
    void *p = mmap(NULL, sizeof(struct steal_flags), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return NULL;
    }
    struct steal_flags *f = p;
    *f = (struct steal_flags){0};
    return f;
}

static bool test_pause_does_not_steal_the_wakeup(void) {
    const char *name;
    size_t len;
    struct instance *inst = open_instance("steal", 1, &name, &len);   /* capacidad 1 */
    CHECK(inst != NULL);

    struct message first = msg_with(1, 1);
    CHECK(sync_put(inst, &first, NULL) == PUT_OK);
    CHECK(buffer_is_full(inst));

    struct steal_flags *f = steal_create();
    CHECK(f != NULL);

    pid_t paused[PAUSERS], blocker;

    for (int i = 0; i < PAUSERS; i++) {
        paused[i] = fork();
        CHECK(paused[i] != -1);
        if (paused[i] == 0) {
            alarm(20);
            struct instance *mine;
            size_t ml;
            if (memory_open(name, &mine, &ml) != MEM_OK) {
                _exit(2);
            }
            f->paused_started[i] = 1;
            sync_pause_ms(mine, 5000.0);      /* pausa larga */
            f->paused_done[i] = 1;
            memory_unmap(mine, ml);
            _exit(0);
        }
    }

    blocker = fork();
    CHECK(blocker != -1);
    if (blocker == 0) {
        alarm(20);
        struct instance *mine;
        size_t ml;
        if (memory_open(name, &mine, &ml) != MEM_OK) {
            _exit(2);
        }
        struct message m = msg_with(2, 1);
        f->blocker_started = 1;
        uint64_t t0 = now_monotonic_ns();
        enum put_result r = sync_put(mine, &m, NULL);   /* bloquea: lleno */
        f->blocker_ms = (now_monotonic_ns() - t0) / 1000000ull;
        f->blocker_ok = (r == PUT_OK);
        f->blocker_done = 1;
        memory_unmap(mine, ml);
        _exit(0);
    }

    /* Esperar a que los seis esten en posicion. */
    for (;;) {
        int listos = f->blocker_started;
        for (int i = 0; i < PAUSERS; i++) {
            listos += f->paused_started[i];
        }
        if (listos == PAUSERS + 1) {
            break;
        }
        struct timespec ms = {0, 1000000};
        nanosleep(&ms, NULL);
    }
    struct timespec settle = {0, 150 * 1000000};
    nanosleep(&settle, NULL);                  /* que lleguen a dormirse */
    CHECK(f->blocker_done == 0);               /* sigue bloqueado por espacio */

    /* Un solo get: libera una casilla y emite UN signal en not_full. */
    struct message out;
    CHECK(sync_get(inst, &out, NULL) == GET_OK);

    int st;
    CHECK(waitpid(blocker, &st, 0) == blocker);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(f->blocker_done == 1);
    CHECK(f->blocker_ok);                      /* inserto */
    CHECK(f->blocker_ms < 2000);               /* no espero los 5 s de las pausas */

    /* Y las pausas seguian corriendo: el escenario era el que se queria. */
    int pausas_terminadas = 0;
    for (int i = 0; i < PAUSERS; i++) {
        pausas_terminadas += f->paused_done[i];
    }
    CHECK(pausas_terminadas == 0);

    /* Soltar a los que estan en pausa. */
    sync_lock(inst);
    inst->state = STATE_CLOSING;
    sync_broadcast_all(inst);
    sync_unlock(inst);

    bool ok = true;
    for (int i = 0; i < PAUSERS; i++) {
        if (waitpid(paused[i], &st, 0) != paused[i] || !WIFEXITED(st) ||
            WEXITSTATUS(st) != 0) {
            ok = false;
        }
    }
    CHECK(ok);

    munmap(f, sizeof(*f));
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
        {"snapshot_without_contention", test_snapshot_without_contention},
        {"put_blocks_and_counts_the_wait", test_put_blocks_and_counts_the_wait},
        {"many_processes_conserve_every_message", test_many_processes_conserve_every_message},
        {"pause_actually_waits", test_pause_actually_waits},
        {"pause_skips_when_already_closing", test_pause_skips_when_already_closing},
        {"pause_zero_does_not_wait", test_pause_zero_does_not_wait},
        {"pause_interrupted_by_closing", test_pause_interrupted_by_closing},
        {"pause_does_not_steal_the_wakeup", test_pause_does_not_steal_the_wakeup},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
