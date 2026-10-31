/* test_validation.c — caso "Validación" del enunciado (Issue #19).
 *
 * Todo con los ejecutables reales: argumentos ausentes, capacidad 0, no
 * numéricos, fuera de rango, id duplicado e instancia inexistente. Para
 * cada rechazo se exige lo que pide el enunciado: código distinto de cero,
 * un mensaje claro en stderr, y que no se corrompa otra ejecución (la
 * instancia viva no cambia y sus participantes terminan bien, con un log
 * que ./verify acepta).
 */
#include "e2e_util.h"
#include "memory.h"
#include "participants.h"
#include "sync.h"
#include "test_util.h"

#define P(tag, who) e2e_path("t19v", (tag), (who))

/* Huella de lo que un rechazo no debe tocar en una instancia viva. */
struct footprint {
    int state;
    uint32_t count, active_producers, active_consumers;
    uint32_t registered_producers, registered_consumers;
    uint64_t enqueue_seq, dequeue_seq;
};

static bool take_footprint(const char *name, struct footprint *fp) {
    struct instance *inst;
    size_t len;
    if (memory_open(name, &inst, &len) != MEM_OK)
        return false;
    sync_lock(inst);
    *fp = (struct footprint){
        .state = inst->state, .count = inst->count,
        .active_producers = inst->active_producers,
        .active_consumers = inst->active_consumers,
        .registered_producers = inst->producers.used,
        .registered_consumers = inst->consumers.used,
        .enqueue_seq = inst->enqueue_seq, .dequeue_seq = inst->dequeue_seq,
    };
    sync_unlock(inst);
    memory_unmap(inst, len);
    return true;
}

static bool same_footprint(const struct footprint *a, const struct footprint *b) {
    return memcmp(a, b, sizeof(*a)) == 0;
}

/* Espera hasta 5 s a que la instancia tenga n consumidores activos. */
static bool wait_consumers(const char *name, uint32_t n) {
    for (int i = 0; i < 500; i++) {
        struct footprint fp;
        if (take_footprint(name, &fp) && fp.active_consumers == n)
            return true;
        e2e_sleep_ms(10);
    }
    return false;
}

/* ---- Argumentos ausentes ---------------------------------------------- */

static bool test_missing_arguments(void) {
    const char *err = P("miss", "err");
    const char *exes[] = { "./creator", "./producer", "./consumer", "./finisher" };
    for (size_t i = 0; i < sizeof(exes) / sizeof(exes[0]); i++) {
        CHECK(e2e_run(err, exes[i], NULL) == 1);
        CHECK(e2e_file_has(err, "falta la opcion obligatoria --name"));
        CHECK(e2e_file_has(err, "uso:"));
    }

    /* Falta solo una: el mensaje nombra justo esa. */
    CHECK(e2e_run(err, "./creator", "--name", "/t19v_x", NULL) == 1);
    CHECK(e2e_file_has(err, "falta la opcion obligatoria --capacity"));
    CHECK(e2e_run(err, "./producer", "--name", "/t19v_x", "--id", "1", "--mean-ms", "0",
                  "--seed", "1", NULL) == 1);
    CHECK(e2e_file_has(err, "falta la opcion obligatoria --log"));
    CHECK(e2e_run(err, "./finisher", "--name", "/t19v_x", NULL) == 1);
    CHECK(e2e_file_has(err, "falta la opcion obligatoria --summary"));

    /* Opción sin valor al final de la línea. */
    CHECK(e2e_run(err, "./creator", "--name", "/t19v_x", "--capacity", NULL) == 1);
    CHECK(e2e_file_has(err, "falta el valor de --capacity"));

    /* Nada de eso creó la instancia. */
    CHECK(!e2e_instance_exists("/t19v_x"));
    return true;
}

/* ---- Capacidad 0, no numéricos, fuera de rango (creator) -------------- */

static bool test_creator_capacity_rejected(void) {
    const char *name = e2e_name("t19v", "cap");
    const char *err = P("cap", "err");
    /* 0 es el caso del enunciado; el resto cubre no numéricos, signo,
     * basura al final, espacios, vacío, fuera de rango y overflow. */
    const char *bad[] = { "0", "abc", "8x", "-1", "+8", " 8", "", "1025",
                          "4294967296", "99999999999999999999", "2.5" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (e2e_run(err, "./creator", "--name", name, "--capacity", bad[i], NULL) != 1) {
            fprintf(stderr, "  --capacity '%s' no se rechazo\n", bad[i]);
            return false;
        }
        CHECK(e2e_file_has(err, "--capacity invalida"));
        CHECK(!e2e_instance_exists(name));      /* rechazo sin recursos a medias */
    }

    /* Los bordes válidos sí crean. */
    CHECK(e2e_run(NULL, "./creator", "--name", name, "--capacity", "1024", NULL) == 0);
    CHECK(e2e_instance_exists(name));
    CHECK(memory_unlink(name) == 0);
    CHECK(e2e_run(NULL, "./creator", "--name", name, "--capacity", "1", NULL) == 0);
    CHECK(memory_unlink(name) == 0);
    return true;
}

static bool test_invalid_names(void) {
    const char *err = P("names", "err"), *summary = P("names", "s");
    const char *bad[] = { "sin_barra", "/", "/a/b", "/con espacio", "/acento_\xc3\xa9",
                          "/1234567890123456789012345678901" /* 31 tras '/' */ };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(e2e_run(err, "./creator", "--name", bad[i], "--capacity", "4", NULL) == 1);
        CHECK(e2e_file_has(err, "--name invalido"));
        CHECK(e2e_run(err, "./finisher", "--name", bad[i], "--summary", summary, NULL) == 1);
        CHECK(e2e_file_has(err, "--name invalido"));
    }
    return true;
}

static bool test_unknown_repeated_and_foreign_options(void) {
    const char *err = P("opts", "err");
    CHECK(e2e_run(err, "./creator", "--name", "/t19v_y", "--capacity", "4", "--bogus", "1", NULL) == 1);
    CHECK(e2e_file_has(err, "opcion desconocida: '--bogus'"));
    CHECK(e2e_run(err, "./creator", "--name", "/t19v_y", "--capacity", "4", "--capacity", "8", NULL) == 1);
    CHECK(e2e_file_has(err, "opcion repetida: --capacity"));
    CHECK(e2e_run(err, "./creator", "--name", "/t19v_y", "--capacity", "4", "--seed", "1", NULL) == 1);
    CHECK(e2e_file_has(err, "la opcion --seed no aplica a este ejecutable"));
    CHECK(!e2e_instance_exists("/t19v_y"));
    return true;
}

/* ---- No numéricos y fuera de rango (producer, consumer) --------------- */

struct bad_value {
    const char *flag, *value, *message;
};

/* Corre exe con todos los argumentos válidos salvo `b`, que reemplaza al
 * de su opción, contra una instancia viva. */
static int run_with_bad(const char *exe, const char *name, const char *log,
                        const char *err, const struct bad_value *b) {
    const char *id = "7", *mean = "0", *seed = "1", *msgs = "1";
    if (strcmp(b->flag, "--id") == 0)       id = b->value;
    if (strcmp(b->flag, "--mean-ms") == 0)  mean = b->value;
    if (strcmp(b->flag, "--seed") == 0)     seed = b->value;
    if (strcmp(b->flag, "--messages") == 0) msgs = b->value;
    if (strcmp(exe, "./producer") == 0)
        return e2e_run(err, exe, "--name", name, "--id", id, "--mean-ms", mean,
                       "--seed", seed, "--messages", msgs, "--log", log, NULL);
    return e2e_run(err, exe, "--name", name, "--id", id, "--mean-ms", mean,
                   "--seed", seed, "--log", log, NULL);
}

static bool test_worker_numbers_rejected(void) {
    const char *name = e2e_name("t19v", "num");
    const char *err = P("num", "err"), *log = P("num", "log");
    CHECK(e2e_run(NULL, "./creator", "--name", name, "--capacity", "4", NULL) == 0);
    struct footprint before, after;
    CHECK(take_footprint(name, &before));

    const struct bad_value common[] = {
        {"--id", "0", "--id invalido"},           {"--id", "abc", "--id invalido"},
        {"--id", "-3", "--id invalido"},          {"--id", "4294967296", "--id invalido"},
        {"--mean-ms", "abc", "--mean-ms invalido"}, {"--mean-ms", "-1", "--mean-ms invalido"},
        {"--mean-ms", "1000.5", "--mean-ms invalido"}, {"--mean-ms", "1e3", "--mean-ms invalido"},
        {"--mean-ms", "nan", "--mean-ms invalido"}, {"--mean-ms", "inf", "--mean-ms invalido"},
        {"--seed", "0", "--seed invalida"},       {"--seed", "x", "--seed invalida"},
    };
    const struct bad_value producer_only[] = {
        {"--messages", "0", "--messages invalido"}, {"--messages", "abc", "--messages invalido"},
        {"--messages", "-5", "--messages invalido"},
    };
    const char *exes[] = { "./producer", "./consumer" };
    for (size_t e = 0; e < 2; e++) {
        for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); i++) {
            if (run_with_bad(exes[e], name, log, err, &common[i]) != 1) {
                fprintf(stderr, "  %s %s '%s' no se rechazo\n", exes[e], common[i].flag, common[i].value);
                return false;
            }
            CHECK(e2e_file_has(err, common[i].message));
            /* La CLI rechaza antes de abrir el log o tocar la instancia. */
            CHECK(!e2e_file_exists(log));
        }
    }
    for (size_t i = 0; i < sizeof(producer_only) / sizeof(producer_only[0]); i++) {
        CHECK(run_with_bad("./producer", name, log, err, &producer_only[i]) == 1);
        CHECK(e2e_file_has(err, producer_only[i].message));
        CHECK(!e2e_file_exists(log));
    }

    CHECK(take_footprint(name, &after));
    CHECK(same_footprint(&before, &after));
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* ---- Instancia inexistente -------------------------------------------- */

static bool test_missing_instance(void) {
    const char *name = e2e_name("t19v", "ghost");
    const char *err = P("ghost", "err"), *summary = P("ghost", "s");
    const char *needle = "no existe una instancia con ese nombre";

    CHECK(e2e_run(err, "./producer", "--name", name, "--id", "1", "--mean-ms", "0", "--seed", "1",
                  "--messages", "1", "--log", P("ghost", "p"), NULL) == 1);
    CHECK(e2e_file_has(err, needle));
    CHECK(e2e_run(err, "./consumer", "--name", name, "--id", "1", "--mean-ms", "0", "--seed", "1",
                  "--log", P("ghost", "c"), NULL) == 1);
    CHECK(e2e_file_has(err, needle));
    CHECK(e2e_run(err, "./finisher", "--name", name, "--summary", summary, NULL) == 1);
    CHECK(e2e_file_has(err, needle));

    /* Ninguno la creó por accidente, y el finisher no dejó resumen. */
    CHECK(!e2e_instance_exists(name));
    CHECK(!e2e_file_exists(summary));
    return true;
}

/* ---- Id duplicado sin corromper la ejecución en curso ----------------- */

/* Un consumidor id 1 en curso y un productor id 1 que ya terminó (D8: el
 * id queda tomado toda la vida de la instancia). Los duplicados se
 * rechazan, incluso el que apunta al MISMO log del consumidor en curso:
 * antes ese rechazo le truncaba el CSV y verify encontraba miles de
 * violaciones. 2000 mensajes alcanzan para que el consumidor ya haya
 * volcado su buffer al archivo cuando llega el duplicado. */
static bool test_duplicate_id_keeps_running_execution(void) {
    const char *name = e2e_name("t19v", "dup");
    const char *err = P("dup", "err");
    const char *c1 = P("dup", "c1.csv"), *p1 = P("dup", "p1.csv"), *p2 = P("dup", "p2.csv");
    const char *f = P("dup", "f.csv"), *s = P("dup", "s.csv");
    CHECK(e2e_run(NULL, "./creator", "--name", name, "--capacity", "4", NULL) == 0);

    pid_t cons = e2e_spawn(NULL, "./consumer", "--name", name, "--id", "1", "--mean-ms", "0",
                           "--seed", "1", "--log", c1, NULL);
    CHECK(wait_consumers(name, 1));
    CHECK(e2e_run(NULL, "./producer", "--name", name, "--id", "1", "--mean-ms", "0", "--seed", "1",
                  "--messages", "2000", "--log", p1, NULL) == 0);

    struct footprint before, after;
    CHECK(take_footprint(name, &before));

    /* Consumidor id 1 con otro log: rechazado por el registro. */
    CHECK(e2e_run(err, "./consumer", "--name", name, "--id", "1", "--mean-ms", "0", "--seed", "2",
                  "--log", P("dup", "c1b.csv"), NULL) == 1);
    CHECK(e2e_file_has(err, "id duplicado"));
    /* Productor id 1 de nuevo: ya terminó, pero el id sigue tomado (D8). */
    CHECK(e2e_run(err, "./producer", "--name", name, "--id", "1", "--mean-ms", "0", "--seed", "2",
                  "--messages", "5", "--log", P("dup", "p1b.csv"), NULL) == 1);
    CHECK(e2e_file_has(err, "id duplicado"));
    /* Consumidor id 1 con el MISMO log del que corre: rechazado antes de
     * truncar, y el archivo del otro queda intacto. */
    CHECK(e2e_run(err, "./consumer", "--name", name, "--id", "1", "--mean-ms", "0", "--seed", "2",
                  "--log", c1, NULL) == 1);
    CHECK(e2e_file_has(err, "otro proceso en ejecucion ya escribe en ese archivo"));

    /* La instancia no cambió: mismos activos, registros y secuencias. */
    CHECK(take_footprint(name, &after));
    CHECK(same_footprint(&before, &after));

    /* Y la ejecución en curso sigue sana hasta el final. */
    CHECK(e2e_run(NULL, "./producer", "--name", name, "--id", "2", "--mean-ms", "0", "--seed", "3",
                  "--messages", "5", "--log", p2, NULL) == 0);
    CHECK(e2e_run(NULL, "./finisher", "--name", name, "--summary", s, "--log", f, NULL) == 0);
    CHECK(e2e_wait(cons, 10) == 0);
    CHECK(e2e_run(NULL, "./verify", p1, p2, c1, f, NULL) == 0);
    CHECK(!e2e_instance_exists(name));
    return true;
}

int main(void) {
    if (access("./creator", X_OK) != 0 || access("./producer", X_OK) != 0 ||
        access("./consumer", X_OK) != 0 || access("./finisher", X_OK) != 0 ||
        access("./verify", X_OK) != 0) {
        fprintf(stderr, "test_validation: faltan los ejecutables "
                        "(correr desde la raiz con make test)\n");
        return 1;
    }
    e2e_install_cleanup();
    alarm(120);   /* tope global; cada proceso hijo tiene además el suyo */
    struct test_case tests[] = {
        {"missing_arguments", test_missing_arguments},
        {"creator_capacity_rejected", test_creator_capacity_rejected},
        {"invalid_names", test_invalid_names},
        {"unknown_repeated_and_foreign_options", test_unknown_repeated_and_foreign_options},
        {"worker_numbers_rejected", test_worker_numbers_rejected},
        {"missing_instance", test_missing_instance},
        {"duplicate_id_keeps_running_execution", test_duplicate_id_keeps_running_execution},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
