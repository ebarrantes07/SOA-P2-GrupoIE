/* test_isolation.c — caso "Aislamiento y reutilización" del enunciado
 * (Issue #19), con los ejecutables reales.
 *
 *   - Dos nombres distintos a la vez no interfieren: cada uno con sus
 *     participantes (incluso con los mismos ids), cerrar uno no toca al
 *     otro, y cada juego de logs pasa ./verify por separado.
 *   - Una creación duplicada se rechaza sin modificar la existente, también
 *     cuando varios creator compiten por el mismo nombre al mismo tiempo.
 *   - Tras el cierre el nombre queda libre y la instancia se puede recrear,
 *     con otra capacidad y con su libro de ids vacío.
 */
#include "e2e_util.h"
#include "memory.h"
#include "participants.h"
#include "sync.h"
#include "test_util.h"

#include <inttypes.h>

#define P(tag, who) e2e_path("t19i", (tag), (who))

/* Lee bajo el mutex los campos que interesan. memory_open no cuenta como
 * conexión (no sube refs), así que esto solo se hace mientras la instancia
 * está viva: el finisher no espera a quien la tiene mapeada así. */
struct peek {
    int state;
    uint32_t capacity, active_producers, active_consumers;
    uint64_t enqueue_seq, dequeue_seq;
};

static bool peek(const char *name, struct peek *out) {
    struct instance *inst;
    size_t len;
    if (memory_open(name, &inst, &len) != MEM_OK)
        return false;
    sync_lock(inst);
    *out = (struct peek){
        .state = inst->state, .capacity = inst->capacity,
        .active_producers = inst->active_producers,
        .active_consumers = inst->active_consumers,
        .enqueue_seq = inst->enqueue_seq, .dequeue_seq = inst->dequeue_seq,
    };
    sync_unlock(inst);
    memory_unmap(inst, len);
    return true;
}

/* Espera hasta 5 s a que haya p productores y c consumidores activos. */
static bool wait_active(const char *name, uint32_t p, uint32_t c) {
    for (int i = 0; i < 500; i++) {
        struct peek k;
        if (peek(name, &k) && k.active_producers == p && k.active_consumers == c)
            return true;
        e2e_sleep_ms(10);
    }
    return false;
}

static pid_t start_consumer(const char *name, const char *id, const char *log) {
    return e2e_spawn(NULL, "./consumer", "--name", name, "--id", id, "--mean-ms", "0",
                     "--seed", id, "--log", log, NULL);
}

/* ---- Dos nombres distintos sin interferencia -------------------------- */

static bool test_two_names_do_not_interfere(void) {
    const char *a = e2e_name("t19i", "A"), *b = e2e_name("t19i", "B");
    const char *a_c = P("A", "c1.csv"), *a_p1 = P("A", "p1.csv"), *a_p2 = P("A", "p2.csv");
    const char *a_f = P("A", "f.csv"), *a_s = P("A", "s.csv");
    const char *b_c = P("B", "c1.csv"), *b_p = P("B", "p1.csv");
    const char *b_f = P("B", "f.csv"), *b_s = P("B", "s.csv");

    CHECK(e2e_run(NULL, "./creator", "--name", a, "--capacity", "4", NULL) == 0);
    CHECK(e2e_run(NULL, "./creator", "--name", b, "--capacity", "16", NULL) == 0);

    /* Los mismos ids en las dos: cada instancia tiene su propio libro. */
    pid_t ac = start_consumer(a, "1", a_c);
    pid_t bc = start_consumer(b, "1", b_c);
    CHECK(wait_active(a, 0, 1) && wait_active(b, 0, 1));

    /* A con cuota fija; B sin cuota, produce hasta su propio cierre. */
    pid_t ap1 = e2e_spawn(NULL, "./producer", "--name", a, "--id", "1", "--mean-ms", "0",
                          "--seed", "11", "--messages", "300", "--log", a_p1, NULL);
    pid_t ap2 = e2e_spawn(NULL, "./producer", "--name", a, "--id", "2", "--mean-ms", "0",
                          "--seed", "12", "--messages", "300", "--log", a_p2, NULL);
    pid_t bp = e2e_spawn(NULL, "./producer", "--name", b, "--id", "1", "--mean-ms", "0.2",
                         "--seed", "21", "--log", b_p, NULL);
    CHECK(e2e_wait(ap1, 15) == 0 && e2e_wait(ap2, 15) == 0);

    struct peek ka, kb1, kb2;
    CHECK(peek(a, &ka));
    CHECK(ka.capacity == 4 && ka.enqueue_seq == 600);   /* nada de B entró en A */
    CHECK(peek(b, &kb1));
    CHECK(kb1.capacity == 16 && kb1.active_producers == 1);

    /* Cerrar A no toca a B: B sigue OPEN, con sus dos procesos vivos, y
     * su productor sigue avanzando después del cierre de A. */
    CHECK(e2e_run(NULL, "./finisher", "--name", a, "--summary", a_s, "--log", a_f, NULL) == 0);
    CHECK(e2e_wait(ac, 10) == 0);
    CHECK(!e2e_instance_exists(a));
    e2e_sleep_ms(200);
    CHECK(peek(b, &kb2));
    CHECK(kb2.state == STATE_OPEN);
    CHECK(kb2.active_producers == 1 && kb2.active_consumers == 1);
    CHECK(kb2.enqueue_seq > kb1.enqueue_seq);
    CHECK(waitpid(bp, NULL, WNOHANG) == 0 && waitpid(bc, NULL, WNOHANG) == 0);

    CHECK(e2e_run(NULL, "./finisher", "--name", b, "--summary", b_s, "--log", b_f, NULL) == 0);
    CHECK(e2e_wait(bp, 10) == 0 && e2e_wait(bc, 10) == 0);

    /* Cada juego de logs es completo y consistente por sí solo: un mensaje
     * de B extraído en A (o al revés) sería una violación. */
    CHECK(e2e_run(NULL, "./verify", a_p1, a_p2, a_c, a_f, NULL) == 0);
    CHECK(e2e_run(NULL, "./verify", b_p, b_c, b_f, NULL) == 0);
    return true;
}

/* ---- Creación duplicada rechazada ------------------------------------- */

static bool test_duplicate_creation_rejected(void) {
    const char *name = e2e_name("t19i", "dup");
    const char *err = P("dup", "err");
    const char *c = P("dup", "c1.csv"), *p = P("dup", "p1.csv");
    const char *f = P("dup", "f.csv"), *s = P("dup", "s.csv");

    CHECK(e2e_run(NULL, "./creator", "--name", name, "--capacity", "4", NULL) == 0);
    pid_t cons = start_consumer(name, "1", c);
    CHECK(wait_active(name, 0, 1));
    CHECK(e2e_run(NULL, "./producer", "--name", name, "--id", "1", "--mean-ms", "0", "--seed", "1",
                  "--messages", "50", "--log", p, NULL) == 0);
    struct peek before, after;
    CHECK(peek(name, &before));

    /* Otra capacidad, la misma, y la mínima: siempre "ya existe". */
    const char *caps[] = { "8", "4", "1" };
    for (size_t i = 0; i < 3; i++) {
        CHECK(e2e_run(err, "./creator", "--name", name, "--capacity", caps[i], NULL) == 1);
        CHECK(e2e_file_has(err, "ya existe una instancia con ese nombre"));
    }

    /* La existente sigue igual y operando. */
    CHECK(peek(name, &after));
    CHECK(after.capacity == 4 && after.state == STATE_OPEN);
    CHECK(after.active_consumers == 1);
    CHECK(after.enqueue_seq == before.enqueue_seq && after.dequeue_seq == before.dequeue_seq);
    CHECK(e2e_run(NULL, "./finisher", "--name", name, "--summary", s, "--log", f, NULL) == 0);
    CHECK(e2e_wait(cons, 10) == 0);
    CHECK(e2e_run(NULL, "./verify", p, c, f, NULL) == 0);
    return true;
}

/* Varios creator a la vez con el mismo nombre: gana exactamente uno, los
 * demás reciben "ya existe", y la instancia queda publicada y usable. Lo
 * garantiza shm_open con O_CREAT | O_EXCL, que el kernel resuelve en un
 * solo paso (D1). */
static bool test_concurrent_creators_one_wins(void) {
    enum { N = 8 };
    for (int round = 0; round < 5; round++) {
        char tag[16];
        snprintf(tag, sizeof(tag), "race%d", round);
        const char *name = e2e_name("t19i", tag);
        char caps[N][8];
        pid_t pids[N];
        for (int i = 0; i < N; i++) {
            snprintf(caps[i], sizeof(caps[i]), "%d", i + 1);
            pids[i] = e2e_spawn(NULL, "./creator", "--name", name, "--capacity", caps[i], NULL);
        }
        int ok = 0, rejected = 0;
        for (int i = 0; i < N; i++) {
            int rc = e2e_wait(pids[i], 10);
            if (rc == 0)
                ok++;
            else if (rc == 1)
                rejected++;
        }
        if (ok != 1 || rejected != N - 1) {
            fprintf(stderr, "  ronda %d: %d creator ganaron y %d fueron rechazados\n",
                    round, ok, rejected);
            return false;
        }
        struct peek k;
        CHECK(peek(name, &k));
        CHECK(k.state == STATE_OPEN && k.capacity >= 1 && k.capacity <= N);
        CHECK(memory_unlink(name) == 0);
    }
    return true;
}

/* ---- Recreación tras el cierre ---------------------------------------- */

/* Una vida completa de la instancia con un productor id 1 y un
 * consumidor id 1, cerrada por el finisher. */
static bool one_life(const char *name, const char *capacity, const char *tag,
                     const char *messages) {
    const char *c = P(tag, "c1.csv"), *p = P(tag, "p1.csv");
    const char *f = P(tag, "f.csv"), *s = P(tag, "s.csv");
    CHECK(e2e_run(NULL, "./creator", "--name", name, "--capacity", capacity, NULL) == 0);
    struct peek k;
    CHECK(peek(name, &k));
    CHECK(k.capacity == (uint32_t)strtoul(capacity, NULL, 10));
    CHECK(k.enqueue_seq == 0 && k.dequeue_seq == 0);      /* arranca de cero */

    pid_t cons = start_consumer(name, "1", c);
    CHECK(wait_active(name, 0, 1));
    CHECK(e2e_run(NULL, "./producer", "--name", name, "--id", "1", "--mean-ms", "0", "--seed", "1",
                  "--messages", messages, "--log", p, NULL) == 0);
    CHECK(e2e_run(NULL, "./finisher", "--name", name, "--summary", s, "--log", f, NULL) == 0);
    CHECK(e2e_wait(cons, 10) == 0);
    CHECK(e2e_run(NULL, "./verify", p, c, f, NULL) == 0);
    return true;
}

static bool test_recreate_after_close(void) {
    const char *name = e2e_name("t19i", "again");
    const char *err = P("again", "err");

    CHECK(one_life(name, "4", "life1", "40"));
    /* El finisher liberó el nombre: ya no se puede conectar nadie... */
    CHECK(!e2e_instance_exists(name));
    CHECK(e2e_run(err, "./consumer", "--name", name, "--id", "9", "--mean-ms", "0", "--seed", "1",
                  "--log", P("again", "late.csv"), NULL) == 1);
    CHECK(e2e_file_has(err, "no existe una instancia con ese nombre"));

    /* ...y se puede recrear con otra capacidad. Los mismos ids vuelven a
     * servir: el libro de D8 es de cada vida, no del nombre. */
    CHECK(one_life(name, "2", "life2", "25"));
    CHECK(one_life(name, "1024", "life3", "10"));
    CHECK(!e2e_instance_exists(name));
    return true;
}

int main(void) {
    if (access("./creator", X_OK) != 0 || access("./producer", X_OK) != 0 ||
        access("./consumer", X_OK) != 0 || access("./finisher", X_OK) != 0 ||
        access("./verify", X_OK) != 0) {
        fprintf(stderr, "test_isolation: faltan los ejecutables "
                        "(correr desde la raiz con make test)\n");
        return 1;
    }
    atexit(e2e_cleanup);
    alarm(120);   /* tope global; cada proceso hijo tiene además el suyo */
    struct test_case tests[] = {
        {"two_names_do_not_interfere", test_two_names_do_not_interfere},
        {"duplicate_creation_rejected", test_duplicate_creation_rejected},
        {"concurrent_creators_one_wins", test_concurrent_creators_one_wins},
        {"recreate_after_close", test_recreate_after_close},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
