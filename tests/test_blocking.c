/* test_blocking.c — casos "buffer lleno" y "buffer vacio" del enunciado
 * (Issue #21). Los dos lanzan ./producer y ./consumer de verdad: ningun
 * extremo lo simula la prueba, a diferencia de test_producer.c y
 * test_consumer.c, donde el proceso de prueba hacia de contraparte.
 *
 * El criterio exigente no es que el proceso avance cuando aparece la
 * contraparte, sino que mientras espera NO consuma CPU: una espera por
 * sondeo (`while (lleno) ;`) tambien "eventualmente avanza" y pasaria una
 * prueba que solo mire el progreso. Por eso cada caso mide tres cosas del
 * proceso bloqueado: que siga vivo, que su contador no avance, y que su
 * CPU no crezca mientras el reloj de pared corre.
 */
#include "clock.h"
#include "logger.h"
#include "memory.h"
#include "participants.h"
#include "sync.h"
#include "test_util.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---- Medicion de CPU por proceso -------------------------------------- */

/* Ventana de observacion y tolerancia. Con CLK_TCK = 100 (tick de 10 ms),
 * un proceso que sondeara 600 ms acumularia ~60 ticks; se toleran 2 (20 ms,
 * 3.3 % de la ventana) para absorber una falta de pagina o un despertar
 * espurio. Margen de 30x entre lo tolerado y lo que daria un sondeo. */
#define OBSERVE_MS      600
#define CPU_TICKS_MAX   2

struct proc_sample {
    unsigned long long cpu_ticks;   /* utime + stime */
    char state;                     /* 'S' durmiendo, 'R' ejecutando, ... */
};

/* Lee /proc/<pid>/stat. Depende de Linux a proposito: es el unico lugar
 * donde se puede ver la CPU de OTRO proceso MIENTRAS esta bloqueado.
 * Alternativas descartadas en D25. Devuelve false si no se pudo leer, y
 * entonces la prueba falla con un mensaje claro en vez de pasar sola. */
static bool sample_proc(pid_t pid, struct proc_sample *out) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return false;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0)
        return false;
    buf[n] = '\0';

    /* El campo 2 es el nombre del ejecutable entre parentesis y puede
     * contener espacios, asi que se parsea desde el ULTIMO ')' y no por
     * numero de campo desde el principio. */
    char *p = strrchr(buf, ')');
    if (p == NULL)
        return false;
    p++;

    char state = 0;
    int consumed = 0;
    if (sscanf(p, " %c%n", &state, &consumed) != 1)
        return false;
    p += consumed;

    /* Tras el estado (campo 3) vienen 10 campos (4..13) antes de utime
     * (14) y stime (15). */
    for (int i = 0; i < 10; i++) {
        long long ignored;
        if (sscanf(p, " %lld%n", &ignored, &consumed) != 1)
            return false;
        p += consumed;
    }
    unsigned long long utime, stime;
    if (sscanf(p, " %llu %llu", &utime, &stime) != 2)
        return false;

    out->cpu_ticks = utime + stime;
    out->state = state;
    return true;
}

/* ---- Utilidades ------------------------------------------------------- */

static void sleep_ms(long ms) {
    struct timespec d = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&d, NULL);
}

static char *path_for(const char *tag, const char *who) {
    static char bufs[8][96];
    static int next;
    char *b = bufs[next++ % 8];
    snprintf(b, sizeof(bufs[0]), "build/tb_%d_%s_%s.csv", (int)getpid(), tag, who);
    return b;
}

/* Los nombres se borran con atexit: CHECK sale en el acto y se saltearia
 * la limpieza, dejando el segmento en /dev/shm (igual que en #14 y #15). */
static char all_names[8][INSTANCE_NAME_MAX + 1];
static int n_names;

static void unlink_all_names(void) {
    for (int i = 0; i < n_names; i++)
        memory_unlink(all_names[i]);
}

static const char *name_for(const char *tag) {
    if (n_names == (int)(sizeof(all_names) / sizeof(all_names[0])))
        abort();
    char *b = all_names[n_names++];
    snprintf(b, sizeof(all_names[0]), "/t21_%d_%s", (int)getpid(), tag);
    memory_unlink(b);
    return b;
}

static pid_t spawn(const char *exe, const char *name, unsigned id,
                   const char *messages, const char *log) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        char idbuf[16];
        snprintf(idbuf, sizeof(idbuf), "%u", id);
        if (messages != NULL)
            execl(exe, exe, "--name", name, "--id", idbuf, "--mean-ms", "0",
                  "--seed", "7", "--messages", messages, "--log", log, (char *)NULL);
        else
            execl(exe, exe, "--name", name, "--id", idbuf, "--mean-ms", "0",
                  "--seed", "7", "--log", log, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static int wait_exit(pid_t pid, int seconds) {
    for (int i = 0; i < seconds * 100; i++) {
        int st;
        if (waitpid(pid, &st, WNOHANG) == pid)
            return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        sleep_ms(10);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

/* Espera a que se cumpla una condicion sobre la instancia, leida bajo el
 * mutex. Devuelve false si no se cumple en `seconds`. */
static bool wait_for(struct instance *inst, bool (*pred)(const struct instance *),
                     int seconds) {
    for (int i = 0; i < seconds * 100; i++) {
        sync_lock(inst);
        bool ok = pred(inst);
        sync_unlock(inst);
        if (ok)
            return true;
        sleep_ms(10);
    }
    return false;
}

static bool buffer_full(const struct instance *inst)   { return inst->count == inst->capacity; }
static bool one_consumer(const struct instance *inst)  { return inst->active_consumers == 1; }
static bool two_producers(const struct instance *inst) { return inst->active_producers == 2; }
static bool two_consumers(const struct instance *inst) { return inst->active_consumers == 2; }

/* El nucleo de las dos pruebas. Durante OBSERVE_MS: TODOS los procesos
 * siguen vivos, el contador compartido no se mueve, y la CPU de cada uno no
 * crece. Los n procesos se miden en la MISMA ventana (una sola espera), asi
 * la prueba no se alarga con cada proceso de mas. */
static bool assert_all_blocked(const pid_t *pids, size_t n, struct instance *inst,
                               const uint64_t *counter, const char *what) {
    struct proc_sample before[4], after[4];
    if (n > sizeof(before) / sizeof(before[0]))
        return false;
    for (size_t i = 0; i < n; i++)
        if (!sample_proc(pids[i], &before[i])) {
            fprintf(stderr, "  no se pudo leer /proc/%ld/stat: estas pruebas requieren Linux (D25)\n",
                    (long)pids[i]);
            return false;
        }

    sync_lock(inst);
    uint64_t counter_before = *counter;
    sync_unlock(inst);

    uint64_t t0 = now_monotonic_ns();
    sleep_ms(OBSERVE_MS);
    uint64_t wall_ns = now_monotonic_ns() - t0;

    for (size_t i = 0; i < n; i++)
        if (!sample_proc(pids[i], &after[i]))
            return false;
    sync_lock(inst);
    uint64_t counter_after = *counter;
    sync_unlock(inst);

    if (counter_after != counter_before) {
        fprintf(stderr, "  %s: el contador avanzo de %llu a %llu sin la contraparte\n",
                what, (unsigned long long)counter_before, (unsigned long long)counter_after);
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        int st;
        if (waitpid(pids[i], &st, WNOHANG) != 0) {
            fprintf(stderr, "  %s: el proceso %zu termino en vez de esperar\n", what, i + 1);
            return false;
        }
        unsigned long long used = after[i].cpu_ticks - before[i].cpu_ticks;
        if (used > CPU_TICKS_MAX) {
            fprintf(stderr, "  %s: el proceso %zu consumio %llu ticks de CPU en %llu ms "
                            "de pared: parece sondeo\n",
                    what, i + 1, used, (unsigned long long)(wall_ns / 1000000));
            return false;
        }
        /* 'S' espera interrumpible (futex), 'D' no interrumpible. */
        if (after[i].state != 'S' && after[i].state != 'D') {
            fprintf(stderr, "  %s: el proceso %zu en estado '%c' y no 'S': no esta durmiendo\n",
                    what, i + 1, after[i].state);
            return false;
        }
    }
    return true;
}

/* Corte a CLOSING con su fila, para que los procesos puedan terminar. */
static bool cut(struct instance *inst, struct csv_logger *f) {
    struct close_snapshot snap;
    if (closing_begin(inst, &snap) != CLOSE_OK)
        return false;
    struct csv_record r;
    csv_record_event(&r, CSV_CLOSING, snap.closing_ns, NULL);
    r.enqueue_seq = snap.accepted;
    r.dequeue_seq = snap.extracted;
    r.occupancy = snap.pending;
    return csv_logger_write(f, &r) == 0;
}

static int run_verify4(const char *a, const char *b, const char *c, const char *d) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "./verify %s %s %s %s > /dev/null 2>&1", a, b, c, d);
    int st = system(cmd);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* ---- Caso "buffer lleno" ---------------------------------------------- */

/* Productor sin consumidores: se bloquea al llenarse. Al iniciar un
 * consumidor, reanuda y completa su cuota. */
static bool test_full_buffer_blocks_then_resumes(void) {
    const char *name = name_for("full");
    struct instance *inst;
    size_t len;
    CHECK(memory_create(name, 4, &inst, &len) == MEM_OK);

    char *plog = path_for("full", "p1"), *plog2 = path_for("full", "p2");
    char *clog = path_for("full", "c"), *flog = path_for("full", "f");
    struct csv_logger f;
    CHECK(csv_logger_open(&f, flog, CSV_FINISHER, 0) == 0);

    /* Dos productores, como dice el criterio: los dos tienen que quedar
     * dormidos, no solo el que llego primero. */
    pid_t p[2];
    p[0] = spawn("./producer", name, 1, "20", plog);
    p[1] = spawn("./producer", name, 2, "20", plog2);
    CHECK(wait_for(inst, two_producers, 5));
    CHECK(wait_for(inst, buffer_full, 5));        /* lleno: 4 de 4 */

    /* Bloqueados por espacio, sin consumidores. */
    CHECK(assert_all_blocked(p, 2, inst, &inst->enqueue_seq, "buffer lleno"));
    sync_lock(inst);
    CHECK(inst->count == 4 && inst->enqueue_seq == 4);
    sync_unlock(inst);

    /* Aparece el consumidor: tienen que reanudar y terminar sus cuotas. */
    pid_t c = spawn("./consumer", name, 1, NULL, clog);
    CHECK(wait_for(inst, one_consumer, 5));
    CHECK(wait_exit(p[0], 15) == 0);
    CHECK(wait_exit(p[1], 15) == 0);
    sync_lock(inst);
    CHECK(inst->enqueue_seq == 40);               /* 20 + 20 */
    sync_unlock(inst);

    CHECK(cut(inst, &f));
    CHECK(wait_exit(c, 15) == 0);
    CHECK(csv_logger_close(&f) == 0);
    CHECK(run_verify4(plog, plog2, clog, flog) == 0);

    unlink(plog); unlink(plog2); unlink(clog); unlink(flog);
    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* ---- Caso "buffer vacio" ---------------------------------------------- */

/* Consumidor sin productores: espera bloqueado. Al iniciar un productor,
 * avanza. */
static bool test_empty_buffer_blocks_then_resumes(void) {
    const char *name = name_for("empty");
    struct instance *inst;
    size_t len;
    CHECK(memory_create(name, 4, &inst, &len) == MEM_OK);

    char *plog = path_for("empty", "p"), *clog = path_for("empty", "c1");
    char *clog2 = path_for("empty", "c2"), *flog = path_for("empty", "f");
    struct csv_logger f;
    CHECK(csv_logger_open(&f, flog, CSV_FINISHER, 0) == 0);

    pid_t c[2];
    c[0] = spawn("./consumer", name, 1, NULL, clog);
    c[1] = spawn("./consumer", name, 2, NULL, clog2);
    CHECK(wait_for(inst, two_consumers, 5));

    /* Bloqueados por datos, sin productores. */
    CHECK(assert_all_blocked(c, 2, inst, &inst->dequeue_seq, "buffer vacio"));
    sync_lock(inst);
    CHECK(inst->count == 0 && inst->dequeue_seq == 0);
    sync_unlock(inst);

    /* Aparece el productor: tienen que avanzar. */
    pid_t p = spawn("./producer", name, 1, "20", plog);
    CHECK(wait_exit(p, 15) == 0);
    CHECK(cut(inst, &f));
    CHECK(wait_exit(c[0], 15) == 0);
    CHECK(wait_exit(c[1], 15) == 0);
    sync_lock(inst);
    CHECK(inst->dequeue_seq == 20);               /* extrajo todo */
    sync_unlock(inst);

    CHECK(csv_logger_close(&f) == 0);
    CHECK(run_verify4(plog, clog, clog2, flog) == 0);

    unlink(plog); unlink(clog); unlink(clog2); unlink(flog);
    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

int main(void) {
    if (access("./producer", X_OK) != 0 || access("./consumer", X_OK) != 0 ||
        access("./verify", X_OK) != 0) {
        fprintf(stderr, "test_blocking: faltan ./producer, ./consumer o ./verify "
                        "(correr desde la raiz con make test)\n");
        return 1;
    }
    atexit(unlink_all_names);
    alarm(60);   /* margen 10x sobre los ~2 s de una corrida sana */
    struct test_case tests[] = {
        {"full_buffer_blocks_then_resumes", test_full_buffer_blocks_then_resumes},
        {"empty_buffer_blocks_then_resumes", test_empty_buffer_blocks_then_resumes},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
