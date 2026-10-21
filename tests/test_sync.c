/* test_sync.c — pruebas de la capa de sincronizacion (D3, D18, Issue #11).
 *
 * Este slice cubre el ciclo de vida de las primitivas y la exclusion
 * mutua; put/get llegan en el siguiente. Las pruebas usan procesos reales
 * (fork), que es lo unico que verifica de verdad PTHREAD_PROCESS_SHARED:
 * un mutex mal configurado funciona perfecto entre hilos y falla entre
 * procesos.
 */
#include "sync.h"
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

int main(void) {
    struct test_case tests[] = {
        {"broadcast_all_succeeds", test_broadcast_all_succeeds},
        {"lock_excludes_across_processes", test_lock_excludes_across_processes},
        {"broadcast_wakes_waiter_on_closing", test_broadcast_wakes_waiter_on_closing},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
