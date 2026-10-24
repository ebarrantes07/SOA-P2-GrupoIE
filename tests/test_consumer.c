/* test_consumer.c — pruebas de punta a punta del ejecutable consumer (Issue
 * #15). La prueba hace de productor (sync_put + payload_build, con su CSV)
 * y de finisher (closing_begin, con su CSV), lanza ./consumer como proceso
 * real y al final pasa los CSV por ./verify (#7). make test compila los
 * ejecutables antes de las pruebas y las corre desde la raíz del repo.
 */
#include "clock.h"
#include "logger.h"
#include "memory.h"
#include "participants.h"
#include "payload.h"
#include "sync.h"
#include "test_util.h"
#include "verify.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---- Utilidades ------------------------------------------------------- */

static void sleep_ms(long ms) {
    struct timespec d = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&d, NULL);
}

static char *path_for(const char *tag, const char *who) {
    static char bufs[8][96];
    static int next;
    char *b = bufs[next++ % 8];
    snprintf(b, sizeof(bufs[0]), "build/tc_%d_%s_%s.csv", (int)getpid(), tag, who);
    return b;
}

static const char *name_for(const char *tag) {
    static char buf[INSTANCE_NAME_MAX + 1];
    snprintf(buf, sizeof(buf), "/t15_%d_%s", (int)getpid(), tag);
    memory_unlink(buf);
    return buf;
}

/* Lanza ./consumer con los argumentos dados; stdout a /dev/null. */
static pid_t spawn_consumer(const char *name, unsigned id, const char *mean_ms,
                            const char *log) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);   /* los rechazos esperados no ensucian la salida */
        if (devnull != -1) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        char idbuf[16];
        snprintf(idbuf, sizeof(idbuf), "%u", id);
        execl("./consumer", "consumer", "--name", name, "--id", idbuf,
              "--mean-ms", mean_ms, "--seed", "11", "--log", log, (char *)NULL);
        _exit(127);
    }
    return pid;
}

/* Espera a que el proceso termine, con plazo. Devuelve su código de
 * salida, o -1 si no terminó a tiempo (y lo mata) o no salió normal. */
static int wait_exit(pid_t pid, int seconds) {
    for (int i = 0; i < seconds * 100; i++) {
        int st;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid)
            return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        sleep_ms(10);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

/* Espera a que haya `n` consumidores activos (la prueba sí puede sondear). */
static bool wait_active_consumers(struct instance *inst, uint32_t n) {
    for (int i = 0; i < 500; i++) {
        sync_lock(inst);
        uint32_t a = inst->active_consumers;
        sync_unlock(inst);
        if (a >= n)
            return true;
        sleep_ms(10);
    }
    return false;
}

/* Inserta los mensajes [from, to] del productor `pid_id` y registra cada
 * PUT. `corrupt` (0 = ninguno) altera el fill de esa secuencia antes de
 * insertarla, como un bug que dañara el contenido. */
static bool produce(struct instance *inst, struct csv_logger *p, uint32_t pid_id,
                    uint64_t from, uint64_t to, uint64_t corrupt) {
    for (uint64_t s = from; s <= to; s++) {
        struct message m;
        memset(&m, 0, sizeof(m));
        m.producer_id = pid_id;
        m.sequence = s;
        payload_build(&m.payload, pid_id, s);
        if (s == corrupt)
            m.payload.fill[0] ^= 0xff;
        struct op_snapshot snap;
        if (sync_put(inst, &m, &snap) != PUT_OK)
            return false;
        struct csv_record r;
        csv_record_put(&r, &m, &snap);
        if (csv_logger_write(p, &r) != 0)
            return false;
    }
    return true;
}

/* Corte a CLOSING con su fila en el CSV del finisher. */
static bool close_and_log(struct instance *inst, struct csv_logger *f,
                          struct close_snapshot *snap) {
    struct csv_record r;
    csv_record_event(&r, CSV_CLOSE_REQUEST, now_monotonic_ns(), NULL);
    csv_logger_write(f, &r);
    if (closing_begin(inst, snap) != CLOSE_OK)
        return false;
    csv_record_event(&r, CSV_CLOSING, snap->closing_ns, NULL);
    r.enqueue_seq = snap->accepted;
    r.dequeue_seq = snap->extracted;
    r.occupancy = snap->pending;
    return csv_logger_write(f, &r) == 0;
}

/* Lee un CSV y cuenta filas por evento; payload_bad = GET con payload_ok 0. */
struct counts { size_t get, processed, drained, leave, join, rejected, payload_bad; char reject_detail[48]; };

static bool count_rows(const char *path, struct counts *c) {
    memset(c, 0, sizeof(*c));
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return false;
    struct log_row *rows;
    size_t n, line;
    enum log_read_result r = log_rows_read(f, &rows, &n, &line);
    fclose(f);
    if (r != LOG_READ_OK)
        return false;
    for (size_t i = 0; i < n; i++) {
        switch (rows[i].event) {
        case LOG_EVENT_GET:       c->get++; c->payload_bad += rows[i].payload_ok == 0; break;
        case LOG_EVENT_PROCESSED: c->processed++; break;
        case LOG_EVENT_DRAINED:   c->drained++; break;
        case LOG_EVENT_LEAVE:     c->leave++; break;
        case LOG_EVENT_JOIN:      c->join++; break;
        case LOG_EVENT_JOIN_REJECTED:
            c->rejected++;
            snprintf(c->reject_detail, sizeof(c->reject_detail), "%s", rows[i].detail);
            break;
        default: break;
        }
    }
    free(rows);
    return true;
}

/* Corre ./verify sobre los CSV dados y devuelve su código de salida. */
static int run_verify(const char *a, const char *b, const char *c, const char *d) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "./verify %s %s %s %s > /dev/null 2>&1", a, b, c, d ? d : "");
    int st = system(cmd);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Escenario común: instancia, logs del productor y del finisher. */
struct env {
    const char *name;
    struct instance *inst;
    size_t len;
    struct csv_logger p, f;
    char *plog, *flog;
};

static bool env_open(struct env *e, const char *tag, uint32_t capacity) {
    e->name = name_for(tag);
    e->plog = path_for(tag, "p");
    e->flog = path_for(tag, "f");
    return memory_create(e->name, capacity, &e->inst, &e->len) == MEM_OK &&
           csv_logger_open(&e->p, e->plog, CSV_PRODUCER, 1) == 0 &&
           csv_logger_open(&e->f, e->flog, CSV_FINISHER, 0) == 0;
}

static void env_close(struct env *e) {
    csv_logger_close(&e->p);
    csv_logger_close(&e->f);
    memory_unmap(e->inst, e->len);
    memory_unlink(e->name);
    unlink(e->plog);
    unlink(e->flog);
}

/* ---- Pruebas ---------------------------------------------------------- */

static bool test_end_to_end_verified(void) {
    struct env e;
    CHECK(env_open(&e, "e2e", 8));
    char *clog = path_for("e2e", "c");
    pid_t c = spawn_consumer(e.name, 1, "0", clog);
    CHECK(wait_active_consumers(e.inst, 1));

    CHECK(produce(e.inst, &e.p, 1, 1, 500, 0));
    struct close_snapshot snap;
    CHECK(close_and_log(e.inst, &e.f, &snap));
    CHECK(wait_exit(c, 10) == 0);
    csv_logger_close(&e.p);
    csv_logger_close(&e.f);

    struct counts n;
    CHECK(count_rows(clog, &n));
    CHECK(n.join == 1 && n.get == 500 && n.processed == 500);
    CHECK(n.payload_bad == 0 && n.drained == 1 && n.leave == 1);
    CHECK(run_verify(e.plog, clog, e.flog, NULL) == 0);
    CHECK(e.inst->active_consumers == 0);       /* se retiró */
    unlink(clog);
    env_close(&e);
    return true;
}

static bool test_blocks_while_empty(void) {
    struct env e;
    CHECK(env_open(&e, "empty", 4));
    char *clog = path_for("empty", "c");
    pid_t c = spawn_consumer(e.name, 1, "0", clog);
    CHECK(wait_active_consumers(e.inst, 1));

    /* Con el buffer vacío y la instancia OPEN, el consumidor espera
     * bloqueado: no termina por encontrar el buffer vacío. */
    sleep_ms(300);
    CHECK(waitpid(c, NULL, WNOHANG) == 0);

    CHECK(produce(e.inst, &e.p, 1, 1, 1, 0));
    struct close_snapshot snap;
    CHECK(close_and_log(e.inst, &e.f, &snap));
    CHECK(wait_exit(c, 10) == 0);

    struct counts n;
    CHECK(count_rows(clog, &n));
    CHECK(n.get == 1 && n.drained == 1);
    unlink(clog);
    env_close(&e);
    return true;
}

static bool test_drains_everything_after_closing(void) {
    struct env e;
    CHECK(env_open(&e, "drain", 32));
    char *clog = path_for("drain", "c");
    pid_t c = spawn_consumer(e.name, 1, "20", clog);   /* lento: ~20 ms por mensaje */
    CHECK(wait_active_consumers(e.inst, 1));

    CHECK(produce(e.inst, &e.p, 1, 1, 20, 0));
    struct close_snapshot snap;
    CHECK(close_and_log(e.inst, &e.f, &snap));
    CHECK(snap.pending > 0);              /* el corte encontró mensajes sin extraer */
    CHECK(wait_exit(c, 20) == 0);
    csv_logger_close(&e.p);
    csv_logger_close(&e.f);

    struct counts n;
    CHECK(count_rows(clog, &n));
    CHECK(n.get == 20 && n.processed == 20);   /* drenó y terminó de procesar todo */
    CHECK(run_verify(e.plog, clog, e.flog, NULL) == 0);
    unlink(clog);
    env_close(&e);
    return true;
}

static bool test_several_consumers(void) {
    enum { CONSUMERS = 4, MESSAGES = 1000 };
    struct env e;
    CHECK(env_open(&e, "many", 8));
    char *clogs[CONSUMERS];
    pid_t pids[CONSUMERS];
    for (unsigned i = 0; i < CONSUMERS; i++) {
        clogs[i] = path_for("many", (const char *[]){"c1", "c2", "c3", "c4"}[i]);
        pids[i] = spawn_consumer(e.name, i + 1, "0", clogs[i]);
    }
    CHECK(wait_active_consumers(e.inst, CONSUMERS));

    CHECK(produce(e.inst, &e.p, 1, 1, MESSAGES, 0));
    struct close_snapshot snap;
    CHECK(close_and_log(e.inst, &e.f, &snap));
    size_t total = 0;
    for (unsigned i = 0; i < CONSUMERS; i++) {
        CHECK(wait_exit(pids[i], 15) == 0);
        struct counts n;
        CHECK(count_rows(clogs[i], &n));
        CHECK(n.drained == 1 && n.leave == 1);
        total += n.get;
    }
    csv_logger_close(&e.p);
    csv_logger_close(&e.f);
    CHECK(total == MESSAGES);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "./verify %s %s %s %s %s %s > /dev/null 2>&1",
             e.plog, clogs[0], clogs[1], clogs[2], clogs[3], e.flog);
    int st = system(cmd);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    for (unsigned i = 0; i < CONSUMERS; i++)
        unlink(clogs[i]);
    env_close(&e);
    return true;
}

static bool test_duplicate_id_rejected(void) {
    struct env e;
    CHECK(env_open(&e, "dupid", 4));
    char *a = path_for("dupid", "a"), *b = path_for("dupid", "b");
    pid_t first = spawn_consumer(e.name, 5, "0", a);
    CHECK(wait_active_consumers(e.inst, 1));

    pid_t second = spawn_consumer(e.name, 5, "0", b);
    CHECK(wait_exit(second, 5) == 1);
    struct counts n;
    CHECK(count_rows(b, &n));
    CHECK(n.rejected == 1 && strcmp(n.reject_detail, "duplicate_id") == 0 && n.join == 0);

    /* El primero no se enteró: sigue y termina normal. */
    CHECK(waitpid(first, NULL, WNOHANG) == 0);
    CHECK(produce(e.inst, &e.p, 1, 1, 10, 0));
    struct close_snapshot snap;
    CHECK(close_and_log(e.inst, &e.f, &snap));
    CHECK(wait_exit(first, 10) == 0);
    CHECK(count_rows(a, &n));
    CHECK(n.get == 10);
    unlink(a);
    unlink(b);
    env_close(&e);
    return true;
}

static bool test_missing_instance(void) {
    char *clog = path_for("missing", "c");
    pid_t c = spawn_consumer(name_for("nadie"), 1, "0", clog);
    CHECK(wait_exit(c, 5) == 1);
    struct counts n;
    CHECK(count_rows(clog, &n));
    CHECK(n.rejected == 1 && strcmp(n.reject_detail, "not_found") == 0);
    unlink(clog);
    return true;
}

static bool test_join_after_closing_rejected(void) {
    struct env e;
    CHECK(env_open(&e, "late", 4));
    struct close_snapshot snap;
    CHECK(close_and_log(e.inst, &e.f, &snap));   /* sin trabajadores: el cierre procede */

    char *clog = path_for("late", "c");
    pid_t c = spawn_consumer(e.name, 1, "0", clog);
    CHECK(wait_exit(c, 5) == 1);
    struct counts n;
    CHECK(count_rows(clog, &n));
    CHECK(n.rejected == 1 && strcmp(n.reject_detail, "closing") == 0);
    unlink(clog);
    env_close(&e);
    return true;
}

static bool test_corrupted_payload_flagged(void) {
    struct env e;
    CHECK(env_open(&e, "bad", 8));
    char *clog = path_for("bad", "c");
    pid_t c = spawn_consumer(e.name, 1, "0", clog);
    CHECK(wait_active_consumers(e.inst, 1));

    CHECK(produce(e.inst, &e.p, 1, 1, 30, 17));    /* la 17 llega con el fill dañado */
    struct close_snapshot snap;
    CHECK(close_and_log(e.inst, &e.f, &snap));
    CHECK(wait_exit(c, 10) == 0);
    csv_logger_close(&e.p);
    csv_logger_close(&e.f);

    struct counts n;
    CHECK(count_rows(clog, &n));
    CHECK(n.get == 30 && n.payload_bad == 1);
    CHECK(run_verify(e.plog, clog, e.flog, NULL) == 1);   /* el verificador lo reporta */
    unlink(clog);
    env_close(&e);
    return true;
}

int main(void) {
    if (access("./consumer", X_OK) != 0 || access("./verify", X_OK) != 0) {
        fprintf(stderr, "test_consumer: faltan ./consumer o ./verify (correr desde la raiz con make test)\n");
        return 1;
    }
    struct test_case tests[] = {
        {"end_to_end_verified", test_end_to_end_verified},
        {"blocks_while_empty", test_blocks_while_empty},
        {"drains_everything_after_closing", test_drains_everything_after_closing},
        {"several_consumers", test_several_consumers},
        {"duplicate_id_rejected", test_duplicate_id_rejected},
        {"missing_instance", test_missing_instance},
        {"join_after_closing_rejected", test_join_after_closing_rejected},
        {"corrupted_payload_flagged", test_corrupted_payload_flagged},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
