/* test_producer.c — pruebas de punta a punta del ejecutable producer
 * (Issue #14). La prueba hace de consumidor (se registra, drena con
 * sync_get, recalcula el payload y escribe su CSV) y de finisher
 * (closing_begin con su CSV), lanza ./producer como proceso real y al final
 * pasa los CSV por ./verify (#16).
 *
 * El registro como consumidor no es decorativo: sin un consumidor activo,
 * closing_begin rechaza el corte con CLOSE_NO_CONSUMER mientras quede
 * trabajo pendiente (precondicion de D10).
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
    snprintf(b, sizeof(bufs[0]), "build/tp_%d_%s_%s.csv", (int)getpid(), tag, who);
    return b;
}

/* Todo nombre entregado se anota aqui para borrarlo al terminar el
 * proceso, pase lo que pase. Hace falta porque CHECK devuelve false en el
 * acto: una prueba que falla a mitad se saltea su env_close y dejaria el
 * segmento en /dev/shm hasta el proximo reinicio. Medido: forzando una
 * falla, sin esto queda un t14_* colgado. */
static char created[16][INSTANCE_NAME_MAX + 1];
static size_t created_n;

static void unlink_created_names(void) {
    for (size_t i = 0; i < created_n; i++)
        memory_unlink(created[i]);
}

/* Un buffer por llamada: el nombre se guarda en struct env y tiene que
 * sobrevivir a otras llamadas dentro de la misma prueba. */
static const char *name_for(const char *tag) {
    static char bufs[4][INSTANCE_NAME_MAX + 1];
    static int next;
    char *b = bufs[next++ % 4];
    snprintf(b, sizeof(bufs[0]), "/t14_%d_%s", (int)getpid(), tag);
    memory_unlink(b);                 /* restos de una corrida anterior */
    if (created_n < sizeof(created) / sizeof(created[0]))
        snprintf(created[created_n++], sizeof(created[0]), "%s", b);
    return b;
}

/* Lanza ./producer. `messages` en NULL omite --messages (sin cuota). */
static pid_t spawn_producer(const char *name, unsigned id, const char *mean_ms,
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
            execl("./producer", "producer", "--name", name, "--id", idbuf,
                  "--mean-ms", mean_ms, "--seed", "7", "--messages", messages,
                  "--log", log, (char *)NULL);
        else
            execl("./producer", "producer", "--name", name, "--id", idbuf,
                  "--mean-ms", mean_ms, "--seed", "7", "--log", log, (char *)NULL);
        _exit(127);
    }
    return pid;
}

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

static bool wait_active_producers(struct instance *inst, uint32_t n) {
    for (int i = 0; i < 500; i++) {
        sync_lock(inst);
        uint32_t a = inst->active_producers;
        sync_unlock(inst);
        if (a >= n)
            return true;
        sleep_ms(10);
    }
    return false;
}

/* Filas del CSV del productor que interesan, ya parseadas. */
struct pcounts {
    size_t put, put_rejected, join, leave, rejected;
    uint64_t last_put_seq;        /* sequence del ultimo PUT */
    uint64_t rejected_seq;        /* sequence del PUT_REJECTED, 0 si no hubo */
    bool rejected_seq_also_put;   /* esa sequence aparecio tambien como PUT */
    char leave_detail[CSV_DETAIL_MAX];
    char reject_detail[CSV_DETAIL_MAX];
};

static bool count_rows(const char *path, struct pcounts *c) {
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
        case LOG_EVENT_PUT:
            c->put++;
            c->last_put_seq = rows[i].sequence;
            break;
        case LOG_EVENT_PUT_REJECTED:
            c->put_rejected++;
            c->rejected_seq = rows[i].sequence;
            snprintf(c->reject_detail, sizeof(c->reject_detail), "%s", rows[i].detail);
            break;
        case LOG_EVENT_JOIN: c->join++; break;
        case LOG_EVENT_LEAVE:
            c->leave++;
            snprintf(c->leave_detail, sizeof(c->leave_detail), "%s", rows[i].detail);
            break;
        case LOG_EVENT_JOIN_REJECTED:
            c->rejected++;
            snprintf(c->reject_detail, sizeof(c->reject_detail), "%s", rows[i].detail);
            break;
        default: break;
        }
    }
    /* La secuencia del intento rechazado no debe aparecer como PUT. */
    if (c->rejected_seq != 0)
        for (size_t i = 0; i < n; i++)
            if (rows[i].event == LOG_EVENT_PUT && rows[i].sequence == c->rejected_seq)
                c->rejected_seq_also_put = true;
    free(rows);
    return true;
}

static int run_verify(const char *a, const char *b, const char *c) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "./verify %s %s %s > /dev/null 2>&1", a, b, c);
    int st = system(cmd);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* ---- Escenario: la prueba como consumidor y como finisher ------------- */

struct env {
    const char *name;
    struct instance *inst;
    size_t len;
    int cslot;
    struct csv_logger c, f;
    char *clog, *flog;
    struct participant_stats stats;
};

static bool env_open(struct env *e, const char *tag, uint32_t capacity) {
    memset(e, 0, sizeof(*e));
    e->name = name_for(tag);
    e->clog = path_for(tag, "c");
    e->flog = path_for(tag, "f");
    if (memory_create(e->name, capacity, &e->inst, &e->len) != MEM_OK)
        return false;
    if (participant_register(e->inst, ROLE_CONSUMER, 1, &e->cslot) != REG_OK)
        return false;
    return csv_logger_open(&e->c, e->clog, CSV_CONSUMER, 1) == 0 &&
           csv_logger_open(&e->f, e->flog, CSV_FINISHER, 0) == 0;
}

static void env_close(struct env *e) {
    participant_leave(e->inst, ROLE_CONSUMER, e->cslot, &e->stats);
    csv_logger_close(&e->c);
    csv_logger_close(&e->f);
    memory_unmap(e->inst, e->len);
    memory_unlink(e->name);
    unlink(e->clog);
    unlink(e->flog);
}

/* Extrae `n` mensajes (o hasta el drenaje si n == 0), verificando el
 * payload y escribiendo las filas GET. Devuelve cuantos extrajo, o -1 si
 * algun payload no verifico. */
static long drain(struct env *e, long n) {
    long got = 0;
    for (;;) {
        if (n > 0 && got == n)
            return got;
        struct message msg;
        struct op_snapshot snap;
        if (sync_get(e->inst, &msg, &snap) == GET_DRAINED)
            return got;
        if (!payload_verify(&msg))
            return -1;
        e->stats.ops++;
        e->stats.wait_ns += snap.wait_ns;
        e->stats.wait_count += snap.wait_count;
        struct csv_record r;
        csv_record_get(&r, &msg, &snap, true);
        if (csv_logger_write(&e->c, &r) != 0)
            return -1;
        got++;
    }
}

static bool cut(struct env *e, struct close_snapshot *snap) {
    struct csv_record r;
    csv_record_event(&r, CSV_CLOSE_REQUEST, now_monotonic_ns(), NULL);
    csv_logger_write(&e->f, &r);
    if (closing_begin(e->inst, snap) != CLOSE_OK)
        return false;
    csv_record_event(&r, CSV_CLOSING, snap->closing_ns, NULL);
    r.enqueue_seq = snap->accepted;
    r.dequeue_seq = snap->extracted;
    r.occupancy = snap->pending;
    return csv_logger_write(&e->f, &r) == 0;
}

/* ---- Pruebas ---------------------------------------------------------- */

/* Cuota cumplida: exactamente N filas PUT, N extracciones y ./verify
 * certifica los tres archivos. */
static bool test_quota_end_to_end(void) {
    struct env e;
    CHECK(env_open(&e, "quota", 8));
    char *plog = path_for("quota", "p");
    pid_t p = spawn_producer(e.name, 1, "0", "500", plog);

    CHECK(drain(&e, 500) == 500);
    CHECK(wait_exit(p, 10) == 0);

    struct close_snapshot snap;
    CHECK(cut(&e, &snap));
    CHECK(snap.accepted == 500 && snap.extracted == 500 && snap.pending == 0);
    CHECK(drain(&e, 0) == 0);          /* ya no queda nada */
    csv_logger_close(&e.c);
    csv_logger_close(&e.f);

    struct pcounts n;
    CHECK(count_rows(plog, &n));
    CHECK(n.join == 1 && n.put == 500 && n.put_rejected == 0 && n.leave == 1);
    CHECK(strcmp(n.leave_detail, "quota_done") == 0);
    CHECK(n.last_put_seq == 500);      /* la secuencia por productor empieza en 1 */
    CHECK(e.inst->enqueue_seq == 500);
    CHECK(run_verify(plog, e.clog, e.flog) == 0);
    unlink(plog);
    env_close(&e);
    return true;
}

/* Con --mean-ms 0 el productor no duerme, asi que su primera noticia del
 * corte es el rechazo del put: exactamente una fila PUT_REJECTED, y su
 * secuencia no aparece como PUT. */
static bool test_mean_zero_notices_at_put(void) {
    struct env e;
    CHECK(env_open(&e, "mzero", 8));
    char *plog = path_for("mzero", "p");
    pid_t p = spawn_producer(e.name, 1, "0", NULL, plog);   /* sin cuota */
    CHECK(wait_active_producers(e.inst, 1));

    CHECK(drain(&e, 200) == 200);      /* corre un rato */
    struct close_snapshot snap;
    CHECK(cut(&e, &snap));
    CHECK(drain(&e, 0) >= 0);          /* lo que quedo en el buffer */
    CHECK(wait_exit(p, 10) == 0);      /* interrumpido no es error */
    csv_logger_close(&e.c);
    csv_logger_close(&e.f);

    struct pcounts n;
    CHECK(count_rows(plog, &n));
    CHECK(n.put_rejected == 1);
    CHECK(strcmp(n.reject_detail, "closing") == 0);
    CHECK(strcmp(n.leave_detail, "closing") == 0);
    CHECK(n.rejected_seq == n.last_put_seq + 1);   /* consumio un numero */
    CHECK(!n.rejected_seq_also_put);               /* y no quedo como aceptado */
    CHECK(n.put == e.inst->enqueue_seq);           /* aceptados == filas PUT */
    CHECK(run_verify(plog, e.clog, e.flog) == 0);  /* el PUT_REJECTED no es violacion */
    unlink(plog);
    env_close(&e);
    return true;
}

/* Con pausa, el corte lo despierta ahi y se retira SIN intentar: ninguna
 * fila PUT_REJECTED.
 *
 * Capacidad 32 y ~20 ms por mensaje para que nunca se llene el buffer en
 * la ventana de la prueba: asi el productor esta en la pausa y no
 * bloqueado por espacio. La ventana en la que el corte podria pescarlo
 * dentro de sync_put es del orden de un microsegundo contra 20 ms de
 * pausa (~1 en 20 000). */
static bool test_interrupted_pause_leaves_without_attempt(void) {
    struct env e;
    CHECK(env_open(&e, "pause", 32));
    char *plog = path_for("pause", "p");
    pid_t p = spawn_producer(e.name, 1, "20", NULL, plog);
    CHECK(wait_active_producers(e.inst, 1));

    CHECK(drain(&e, 3) == 3);          /* ~60 ms: ya produjo algo */
    struct close_snapshot snap;
    CHECK(cut(&e, &snap));
    CHECK(drain(&e, 0) >= 0);
    CHECK(wait_exit(p, 10) == 0);
    csv_logger_close(&e.c);
    csv_logger_close(&e.f);

    struct pcounts n;
    CHECK(count_rows(plog, &n));
    CHECK(n.put_rejected == 0);                    /* se retiro sin intentar */
    CHECK(strcmp(n.leave_detail, "closing") == 0);
    CHECK(n.put >= 3);
    CHECK(n.put == e.inst->enqueue_seq);
    CHECK(run_verify(plog, e.clog, e.flog) == 0);
    unlink(plog);
    env_close(&e);
    return true;
}

/* No alcanzar la cuota porque llego el cierre no es un error: codigo 0 y
 * LEAVE closing. Lo lee el script del experimento (#24). */
static bool test_unmet_quota_is_not_an_error(void) {
    struct env e;
    CHECK(env_open(&e, "unmet", 8));
    char *plog = path_for("unmet", "p");
    pid_t p = spawn_producer(e.name, 1, "0", "100000000", plog);
    CHECK(wait_active_producers(e.inst, 1));

    CHECK(drain(&e, 100) == 100);
    struct close_snapshot snap;
    CHECK(cut(&e, &snap));
    CHECK(drain(&e, 0) >= 0);
    CHECK(wait_exit(p, 10) == 0);      /* <-- 0, no 1 */

    struct pcounts n;
    CHECK(count_rows(plog, &n));
    CHECK(n.put < 100000000);
    CHECK(strcmp(n.leave_detail, "closing") == 0);
    unlink(plog);
    env_close(&e);
    return true;
}

/* Cuatro productores concurrentes: todas las identidades llegan, el orden
 * global no tiene huecos y ./verify certifica los seis archivos. */
static bool test_several_producers(void) {
    enum { PRODUCERS = 4, EACH = 250 };
    struct env e;
    CHECK(env_open(&e, "many", 8));
    char *plogs[PRODUCERS];
    pid_t pids[PRODUCERS];
    for (unsigned i = 0; i < PRODUCERS; i++) {
        plogs[i] = path_for("many", (const char *[]){"p1", "p2", "p3", "p4"}[i]);
        pids[i] = spawn_producer(e.name, i + 1, "0", "250", plogs[i]);
    }
    CHECK(wait_active_producers(e.inst, PRODUCERS));

    CHECK(drain(&e, PRODUCERS * EACH) == PRODUCERS * EACH);
    for (unsigned i = 0; i < PRODUCERS; i++)
        CHECK(wait_exit(pids[i], 15) == 0);

    struct close_snapshot snap;
    CHECK(cut(&e, &snap));
    CHECK(snap.accepted == PRODUCERS * EACH);
    csv_logger_close(&e.c);
    csv_logger_close(&e.f);

    for (unsigned i = 0; i < PRODUCERS; i++) {
        struct pcounts n;
        CHECK(count_rows(plogs[i], &n));
        CHECK(n.put == EACH && n.last_put_seq == EACH);   /* cada uno cuenta desde 1 */
        CHECK(strcmp(n.leave_detail, "quota_done") == 0);
    }
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "./verify %s %s %s %s %s %s > /dev/null 2>&1",
             plogs[0], plogs[1], plogs[2], plogs[3], e.clog, e.flog);
    int st = system(cmd);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    for (unsigned i = 0; i < PRODUCERS; i++)
        unlink(plogs[i]);
    env_close(&e);
    return true;
}

/* Las metricas por proceso que el resumen del finisher (#18) va a leer:
 * capacidad 1 obliga al productor a dormir en not_full casi en cada
 * mensaje, asi que wait_count tiene que ser > 0. */
static bool test_per_process_metrics_recorded(void) {
    struct env e;
    CHECK(env_open(&e, "stats", 1));
    char *plog = path_for("stats", "p");
    pid_t p = spawn_producer(e.name, 1, "0", "200", plog);

    CHECK(drain(&e, 200) == 200);
    CHECK(wait_exit(p, 10) == 0);

    /* El productor ya se retiro: su casilla tiene las metricas copiadas. */
    const struct participant_slot *s = &e.inst->producers.slots[0];
    CHECK(s->state == SLOT_LEFT);
    CHECK(s->ops == 200);              /* aceptados, no intentos */
    CHECK(s->wait_count > 0);          /* durmio esperando espacio */
    CHECK(s->wait_ns > 0);
    CHECK(s->left_ns >= s->joined_ns);

    struct close_snapshot snap;
    CHECK(cut(&e, &snap));
    unlink(plog);
    env_close(&e);
    return true;
}

static bool test_duplicate_id_rejected(void) {
    struct env e;
    CHECK(env_open(&e, "dupid", 4));
    char *a = path_for("dupid", "a"), *b = path_for("dupid", "b");
    /* Cuota 20 sobre capacidad 4 y nadie drenando todavia: el primero se
     * bloquea con el buffer lleno y sigue registrado mientras el segundo
     * intenta entrar. Con una cuota que quepa en el buffer terminaria en
     * microsegundos y el rechazo no seria por id duplicado. */
    pid_t first = spawn_producer(e.name, 9, "0", "20", a);
    CHECK(wait_active_producers(e.inst, 1));

    pid_t second = spawn_producer(e.name, 9, "0", "4", b);
    CHECK(wait_exit(second, 5) == 1);
    struct pcounts n;
    CHECK(count_rows(b, &n));
    CHECK(n.rejected == 1 && strcmp(n.reject_detail, "duplicate_id") == 0 && n.join == 0);

    CHECK(drain(&e, 20) == 20);
    CHECK(wait_exit(first, 10) == 0);
    struct close_snapshot snap;
    CHECK(cut(&e, &snap));
    unlink(a);
    unlink(b);
    env_close(&e);
    return true;
}

static bool test_missing_instance(void) {
    char *plog = path_for("missing", "p");
    pid_t p = spawn_producer(name_for("nadie"), 1, "0", "1", plog);
    CHECK(wait_exit(p, 5) == 1);
    struct pcounts n;
    CHECK(count_rows(plog, &n));
    CHECK(n.rejected == 1 && strcmp(n.reject_detail, "not_found") == 0);
    unlink(plog);
    return true;
}

/* Instancia ya en CLOSING: la puerta de D9 lo rechaza al conectarse. Sin
 * trabajadores ni mensajes pendientes, el corte procede. */
static bool test_join_after_closing_rejected(void) {
    const char *name = name_for("late");
    struct instance *inst;
    size_t len;
    CHECK(memory_create(name, 4, &inst, &len) == MEM_OK);
    struct close_snapshot snap;
    CHECK(closing_begin(inst, &snap) == CLOSE_OK);

    char *plog = path_for("late", "p");
    pid_t p = spawn_producer(name, 1, "0", "1", plog);
    CHECK(wait_exit(p, 5) == 1);
    struct pcounts n;
    CHECK(count_rows(plog, &n));
    CHECK(n.rejected == 1 && strcmp(n.reject_detail, "closing") == 0);
    CHECK(n.put == 0);
    unlink(plog);
    memory_unmap(inst, len);
    memory_unlink(name);
    return true;
}

int main(void) {
    if (access("./producer", X_OK) != 0 || access("./verify", X_OK) != 0) {
        fprintf(stderr, "test_producer: faltan ./producer o ./verify (correr desde la raiz con make test)\n");
        return 1;
    }
    atexit(unlink_created_names);
    /* Los CSV de build/ se dejan a proposito cuando una prueba falla: son
     * la evidencia de lo que paso, y `make clean` los borra. El segmento
     * de memoria compartida no sirve para depurar y si molesta. */
    struct test_case tests[] = {
        {"quota_end_to_end", test_quota_end_to_end},
        {"mean_zero_notices_at_put", test_mean_zero_notices_at_put},
        {"interrupted_pause_leaves_without_attempt", test_interrupted_pause_leaves_without_attempt},
        {"unmet_quota_is_not_an_error", test_unmet_quota_is_not_an_error},
        {"several_producers", test_several_producers},
        {"per_process_metrics_recorded", test_per_process_metrics_recorded},
        {"duplicate_id_rejected", test_duplicate_id_rejected},
        {"missing_instance", test_missing_instance},
        {"join_after_closing_rejected", test_join_after_closing_rejected},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
