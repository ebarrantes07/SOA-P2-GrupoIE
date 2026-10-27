/* test_finisher.c — pruebas de punta a punta del cierre (Issue #17). Lanzan
 * ./finisher como proceso real. Los participantes los simula la prueba
 * con procesos hijos (y ./producer en el ciclo completo), y al final los
 * CSV pasan por ./verify. Los nombres de instancia se borran con atexit,
 * así una prueba que falla no deja segmentos en /dev/shm.
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

#define MAX_NAMES 32
static char all_names[MAX_NAMES][INSTANCE_NAME_MAX + 1];
static int n_names;

static void unlink_all_names(void) {
    for (int i = 0; i < n_names; i++)
        memory_unlink(all_names[i]);
}

static const char *name_for(const char *tag) {
    if (n_names == MAX_NAMES)
        abort();
    char *b = all_names[n_names++];
    snprintf(b, sizeof(all_names[0]), "/t17_%d_%s", (int)getpid(), tag);
    memory_unlink(b);
    return b;
}

static char *path_for(const char *tag, const char *who) {
    static char bufs[16][96];
    static int next;
    char *b = bufs[next++ % 16];
    snprintf(b, sizeof(bufs[0]), "build/tf_%d_%s_%s.csv", (int)getpid(), tag, who);
    return b;
}

static void sleep_ms(long ms) {
    struct timespec d = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&d, NULL);
}

static void silence(void) {
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull != -1) {
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
    }
}

static pid_t spawn_finisher(const char *name, const char *summary, const char *log) {
    pid_t pid = fork();
    if (pid == 0) {
        silence();
        execl("./finisher", "finisher", "--name", name, "--summary", summary, "--log", log,
              (char *)NULL);
        _exit(127);
    }
    return pid;
}

static pid_t spawn_producer(const char *name, unsigned id, const char *messages, const char *log) {
    pid_t pid = fork();
    if (pid == 0) {
        silence();
        char idbuf[16];
        snprintf(idbuf, sizeof(idbuf), "%u", id);
        execl("./producer", "producer", "--name", name, "--id", idbuf, "--mean-ms", "0",
              "--seed", "7", "--messages", messages, "--log", log, (char *)NULL);
        _exit(127);
    }
    return pid;
}

/* Código de salida con plazo; -1 si no terminó a tiempo (y lo mata). */
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

static bool load_rows(const char *path, struct log_row **rows, size_t *n) {
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return false;
    size_t line;
    enum log_read_result r = log_rows_read(f, rows, n, &line);
    fclose(f);
    return r == LOG_READ_OK;
}

/* Primera fila con ese evento, o NULL. */
static const struct log_row *find_event(const struct log_row *rows, size_t n, enum log_event ev) {
    for (size_t i = 0; i < n; i++)
        if (rows[i].event == ev)
            return &rows[i];
    return NULL;
}

/* Espera (sondeando, en la prueba está bien) a que la instancia deje OPEN. */
static bool wait_not_open(struct instance *inst) {
    for (int i = 0; i < 1000; i++) {
        sync_lock(inst);
        int st = inst->state;
        sync_unlock(inst);
        if (st != STATE_OPEN)
            return true;
        sleep_ms(5);
    }
    return false;
}

/* Participante simulado en un proceso hijo. Avisa por `ready` cuando ya
 * está conectado (y registrado si `reg`), espera el corte, se queda
 * `hold_ms` más y se va. Antes de desconectarse escribe su marca de
 * tiempo por `done`. Sin registro, intenta registrarse tarde y sale con
 * 10 si recibió REG_NOT_OPEN. */
static pid_t spawn_participant(const char *name, bool reg, long hold_ms, int ready[2], int done[2]) {
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    alarm(20);
    close(ready[0]);
    close(done[0]);
    struct instance *inst;
    size_t len;
    if (instance_attach(name, &inst, &len) != ATTACH_OK)
        _exit(2);
    int slot = -1;
    if (reg && participant_register(inst, ROLE_CONSUMER, 77, &slot) != REG_OK)
        _exit(3);
    char c = 'r';
    if (write(ready[1], &c, 1) != 1)
        _exit(4);
    if (!wait_not_open(inst))
        _exit(5);
    sleep_ms(hold_ms);
    int code = 0;
    if (reg) {
        participant_leave(inst, ROLE_CONSUMER, slot, NULL);
    } else {
        int s2;
        code = participant_register(inst, ROLE_CONSUMER, 78, &s2) == REG_NOT_OPEN ? 10 : 6;
    }
    uint64_t t = now_monotonic_ns();
    if (write(done[1], &t, sizeof(t)) != (ssize_t)sizeof(t))
        _exit(7);
    instance_detach(inst, len);
    _exit(code);
}

/* Consumidor simulado que drena y escribe su CSV (para ./verify). */
static pid_t spawn_test_consumer(const char *name, unsigned id, const char *log, int ready[2]) {
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    alarm(20);
    close(ready[0]);
    struct csv_logger lg;
    struct instance *inst;
    size_t len;
    int slot;
    if (csv_logger_open(&lg, log, CSV_CONSUMER, id) != 0 ||
        instance_attach(name, &inst, &len) != ATTACH_OK ||
        participant_register(inst, ROLE_CONSUMER, id, &slot) != REG_OK)
        _exit(2);
    char c = 'r';
    if (write(ready[1], &c, 1) != 1)
        _exit(3);
    struct csv_record r;
    struct message m;
    struct op_snapshot snap;
    while (sync_get(inst, &m, &snap) == GET_OK) {
        csv_record_get(&r, &m, &snap, payload_verify(&m));
        csv_logger_write(&lg, &r);
    }
    csv_record_event(&r, CSV_DRAINED, now_monotonic_ns(), NULL);
    csv_logger_write(&lg, &r);
    participant_leave(inst, ROLE_CONSUMER, slot, NULL);
    instance_detach(inst, len);
    csv_logger_close(&lg);
    _exit(0);
}

static int run_verify(const char *a, const char *b, const char *c) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "./verify %s %s %s > /dev/null 2>&1", a, b, c);
    int st = system(cmd);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static bool create(const char *name, uint32_t capacity) {
    struct instance *inst;
    size_t len;
    if (memory_create(name, capacity, &inst, &len) != MEM_OK)
        return false;
    memory_unmap(inst, len);
    return true;
}

/* ---- Pruebas ---------------------------------------------------------- */

static bool test_close_without_workers(void) {
    const char *name = name_for("idle");
    char *log = path_for("idle", "fin"), *sum = path_for("idle", "sum");
    CHECK(create(name, 4));
    CHECK(wait_exit(spawn_finisher(name, sum, log), 10) == 0);

    struct instance *inst;
    size_t len;
    CHECK(memory_open(name, &inst, &len) == MEM_NOT_FOUND);   /* nombre liberado */

    struct log_row *rows;
    size_t n;
    CHECK(load_rows(log, &rows, &n));
    bool order = n == 4 && rows[0].event == LOG_EVENT_CLOSE_REQUEST &&
                 rows[1].event == LOG_EVENT_CLOSING && rows[2].event == LOG_EVENT_ALL_LEFT &&
                 rows[3].event == LOG_EVENT_CLOSED && strcmp(rows[3].detail, "released") == 0 &&
                 rows[1].enqueue_seq == 0 && rows[1].occupancy == 0;
    free(rows);
    CHECK(order);

    FILE *f = fopen(sum, "r");
    CHECK(f != NULL);
    char text[2048];
    size_t got = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[got] = '\0';
    CHECK(strstr(text, "accepted,instance,0,0,msgs") != NULL);
    CHECK(strstr(text, "invariant_ok,instance,0,1,bool") != NULL);
    CHECK(strstr(text, "close_duration,instance,0,") != NULL);
    unlink(log);
    unlink(sum);
    return true;
}

/* Criterio 1: espera la retirada de los participantes antes de liberar. */
static bool test_waits_for_registered_participant(void) {
    const char *name = name_for("wait");
    char *log = path_for("wait", "fin"), *sum = path_for("wait", "sum");
    CHECK(create(name, 4));
    int ready[2], done[2];
    CHECK(pipe(ready) == 0 && pipe(done) == 0);
    pid_t p = spawn_participant(name, true, 300, ready, done);
    close(ready[1]);
    close(done[1]);
    char c;
    CHECK(read(ready[0], &c, 1) == 1);

    uint64_t t0 = now_monotonic_ns();
    pid_t fin = spawn_finisher(name, sum, log);
    CHECK(wait_exit(fin, 10) == 0);
    uint64_t elapsed = now_monotonic_ns() - t0;
    uint64_t left_at;
    CHECK(read(done[0], &left_at, sizeof(left_at)) == (ssize_t)sizeof(left_at));
    CHECK(wait_exit(p, 5) == 0);

    struct log_row *rows;
    size_t n;
    CHECK(load_rows(log, &rows, &n));
    const struct log_row *all_left = find_event(rows, n, LOG_EVENT_ALL_LEFT);
    bool after = all_left != NULL && all_left->t_ns >= left_at;
    free(rows);
    CHECK(after);                        /* no liberó antes de que se retirara */
    CHECK(elapsed >= 250000000ull);      /* esperó los ~300 ms del participante */
    struct instance *inst;
    size_t len;
    CHECK(memory_open(name, &inst, &len) == MEM_NOT_FOUND);
    unlink(log);
    unlink(sum);
    return true;
}

/* Criterio 2: una conexión en curso (conectado, sin registrar) al cortar. */
static bool test_resolves_connection_in_progress(void) {
    const char *name = name_for("inflight");
    char *log = path_for("inflight", "fin"), *sum = path_for("inflight", "sum");
    CHECK(create(name, 4));
    int ready[2], done[2];
    CHECK(pipe(ready) == 0 && pipe(done) == 0);
    pid_t p = spawn_participant(name, false, 300, ready, done);
    close(ready[1]);
    close(done[1]);
    char c;
    CHECK(read(ready[0], &c, 1) == 1);

    CHECK(wait_exit(spawn_finisher(name, sum, log), 10) == 0);
    uint64_t left_at;
    CHECK(read(done[0], &left_at, sizeof(left_at)) == (ssize_t)sizeof(left_at));
    CHECK(wait_exit(p, 5) == 10);        /* su registro tardío recibió REG_NOT_OPEN */

    struct log_row *rows;
    size_t n;
    CHECK(load_rows(log, &rows, &n));
    const struct log_row *all_left = find_event(rows, n, LOG_EVENT_ALL_LEFT);
    bool after = all_left != NULL && all_left->t_ns >= left_at;
    free(rows);
    CHECK(after);                        /* esperó también a la conexión sin registro */
    unlink(log);
    unlink(sum);
    return true;
}

static bool test_rejected_without_consumer(void) {
    const char *name = name_for("noconsumer");
    char *log = path_for("noconsumer", "fin"), *sum = path_for("noconsumer", "sum");
    CHECK(create(name, 4));

    /* Un productor registrado y sin consumidores: la precondición falla. */
    struct instance *inst;
    size_t len;
    CHECK(instance_attach(name, &inst, &len) == ATTACH_OK);
    int slot;
    CHECK(participant_register(inst, ROLE_PRODUCER, 1, &slot) == REG_OK);

    CHECK(wait_exit(spawn_finisher(name, sum, log), 10) == 1);
    struct log_row *rows;
    size_t n;
    CHECK(load_rows(log, &rows, &n));
    const struct log_row *rej = find_event(rows, n, LOG_EVENT_CLOSE_REJECTED);
    bool rejected = rej != NULL && strcmp(rej->detail, "no_consumer") == 0 &&
                    find_event(rows, n, LOG_EVENT_CLOSING) == NULL;
    free(rows);
    CHECK(rejected);

    /* La instancia quedó como estaba: OPEN, puerta abierta, recursos vivos. */
    CHECK(inst->state == STATE_OPEN);
    CHECK((atomic_load(&inst->refs) & REFS_GATE_CLOSED) == 0);
    CHECK((atomic_load(&inst->refs) & REFS_COUNT_MASK) == 1);   /* solo esta prueba */

    participant_leave(inst, ROLE_PRODUCER, slot, NULL);
    instance_detach(inst, len);
    CHECK(wait_exit(spawn_finisher(name, sum, log), 10) == 0);  /* ahora sí cierra */
    unlink(log);
    unlink(sum);
    return true;
}

static bool test_second_finisher_rejected(void) {
    const char *name = name_for("twofin");
    char *la = path_for("twofin", "a"), *lb = path_for("twofin", "b");
    char *sa = path_for("twofin", "sa"), *sb = path_for("twofin", "sb");
    CHECK(create(name, 4));
    int ready[2], done[2];
    CHECK(pipe(ready) == 0 && pipe(done) == 0);
    pid_t p = spawn_participant(name, true, 800, ready, done);   /* demora el primer cierre */
    close(ready[1]);
    close(done[1]);
    char c;
    CHECK(read(ready[0], &c, 1) == 1);

    struct instance *watch;
    size_t wl;
    CHECK(memory_open(name, &watch, &wl) == MEM_OK);
    pid_t a = spawn_finisher(name, sa, la);
    CHECK(wait_not_open(watch));
    memory_unmap(watch, wl);

    CHECK(wait_exit(spawn_finisher(name, sb, lb), 10) == 1);    /* el segundo se rechaza */
    struct log_row *rows;
    size_t n;
    CHECK(load_rows(lb, &rows, &n));
    const struct log_row *rej = find_event(rows, n, LOG_EVENT_CLOSE_REJECTED);
    bool rejected = rej != NULL && strcmp(rej->detail, "already_closing") == 0;
    free(rows);
    CHECK(rejected);

    CHECK(wait_exit(a, 10) == 0);        /* el primero termina normal */
    CHECK(wait_exit(p, 5) == 0);
    unlink(la);
    unlink(lb);
    unlink(sa);
    unlink(sb);
    return true;
}

/* Un ciclo completo verificado: consumidor de prueba, ./producer y
 * ./finisher; los tres CSV pasan por ./verify. */
static bool full_cycle(const char *name, const char *tag) {
    char *pl = path_for(tag, "p"), *cl = path_for(tag, "c");
    char *fl = path_for(tag, "fin"), *sl = path_for(tag, "sum");
    int ready[2];
    CHECK(pipe(ready) == 0);
    pid_t cons = spawn_test_consumer(name, 1, cl, ready);
    close(ready[1]);
    char c;
    CHECK(read(ready[0], &c, 1) == 1);
    close(ready[0]);

    CHECK(wait_exit(spawn_producer(name, 1, "200", pl), 15) == 0);
    CHECK(wait_exit(spawn_finisher(name, sl, fl), 15) == 0);
    CHECK(wait_exit(cons, 10) == 0);
    CHECK(run_verify(pl, cl, fl) == 0);
    unlink(pl);
    unlink(cl);
    unlink(fl);
    unlink(sl);
    return true;
}

/* Criterio 4: cerrar y volver a crear con el mismo nombre, sin restos. */
static bool test_recreate_after_close(void) {
    const char *name = name_for("again");
    CHECK(create(name, 4));
    CHECK(full_cycle(name, "c1"));

    /* El nombre quedó libre y la instancia nueva arranca limpia. */
    CHECK(create(name, 8));
    struct instance *inst;
    size_t len;
    CHECK(memory_open(name, &inst, &len) == MEM_OK);
    bool fresh = inst->capacity == 8 && inst->state == STATE_OPEN && inst->enqueue_seq == 0 &&
                 inst->dequeue_seq == 0 && atomic_load(&inst->refs) == 0 &&
                 inst->producers.used == 0 && inst->consumers.used == 0 &&
                 inst->metrics.closing_start_ns == 0;
    memory_unmap(inst, len);
    CHECK(fresh);

    /* Y funciona igual que la primera, con los mismos ids de participante. */
    CHECK(full_cycle(name, "c2"));
    return true;
}

int main(void) {
    if (access("./finisher", X_OK) != 0 || access("./producer", X_OK) != 0 ||
        access("./verify", X_OK) != 0) {
        fprintf(stderr, "test_finisher: faltan ejecutables (correr desde la raiz con make test)\n");
        return 1;
    }
    atexit(unlink_all_names);
    struct test_case tests[] = {
        {"close_without_workers", test_close_without_workers},
        {"waits_for_registered_participant", test_waits_for_registered_participant},
        {"resolves_connection_in_progress", test_resolves_connection_in_progress},
        {"rejected_without_consumer", test_rejected_without_consumer},
        {"second_finisher_rejected", test_second_finisher_rejected},
        {"recreate_after_close", test_recreate_after_close},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
