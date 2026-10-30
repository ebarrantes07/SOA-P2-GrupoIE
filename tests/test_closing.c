/* test_closing.c — casos de cierre y carreras de ciclo de vida del
 * enunciado (Issue #22), todos con los ejecutables reales: ./creator,
 * ./producer, ./consumer y ./finisher, y ./verify sobre todos los CSV.
 *
 *   - Cierre durante actividad: buffer lleno y productores bloqueados al
 *     cortar; se drena lo aceptado, todos se retiran y se libera todo.
 *   - Cierre en vacío: consumidores dormidos sin mensajes despiertan y
 *     terminan; también una instancia sin trabajadores.
 *   - Cierre sin consumidor: se rechaza sin dejar OPEN, y al agregar un
 *     consumidor el cierre procede.
 *   - Carreras: incorporaciones mientras el cierre está en curso, y varios
 *     finalizadores a la vez. Lo que se exige es que nadie use un recurso
 *     destruido ni se destruya algo dos veces: un mutex destruido aborta
 *     el proceso (D18), así que cualquier proceso que termine por señal
 *     hace fallar la prueba.
 *
 * test_finisher.c (#17, #50) prueba el mismo protocolo con participantes
 * simulados dentro del proceso de prueba; acá los participantes son los
 * ejecutables que corre el usuario.
 */
#include "clock.h"
#include "e2e_util.h"
#include "memory.h"
#include "participants.h"
#include "test_util.h"

#include <inttypes.h>

#define P(tag, who) e2e_path("t22", (tag), (who))
#define LATE_MAX 64

static pid_t start_consumer(const char *name, const char *id, const char *mean, const char *log) {
    return e2e_spawn(NULL, "./consumer", "--name", name, "--id", id, "--mean-ms", mean,
                     "--seed", id, "--log", log, NULL);
}

/* Productor sin cuota: produce hasta el cierre. */
static pid_t start_producer(const char *name, const char *id, const char *mean, const char *log) {
    return e2e_spawn(NULL, "./producer", "--name", name, "--id", id, "--mean-ms", mean,
                     "--seed", id, "--log", log, NULL);
}

static pid_t start_finisher(const char *name, const char *out, const char *sum, const char *log) {
    return e2e_spawn_all(out, "./finisher", "--name", name, "--summary", sum, "--log", log, NULL);
}

/* Columna `col` (desde 0) de la primera fila del CSV con ese evento.
 * Devuelve false si no hay fila o la columna está vacía. */
static bool csv_event_field(const char *path, const char *event, int col, uint64_t *out) {
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return false;
    char line[512], needle[40];
    snprintf(needle, sizeof(needle), ",%s,", event);
    bool found = false;
    while (!found && fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, needle) == NULL)
            continue;
        char *p = line;
        for (int i = 0; i < col && p != NULL; i++) {
            p = strchr(p, ',');
            if (p != NULL)
                p++;
        }
        if (p != NULL && *p >= '0' && *p <= '9') {
            *out = strtoull(p, NULL, 10);
            found = true;
        }
    }
    fclose(f);
    return found;
}

#define COL_OCCUPANCY 9

static bool has_consumer(const struct e2e_peek *k)  { return k->active_consumers >= 1; }
static bool not_open(const struct e2e_peek *k)      { return k->state != STATE_OPEN; }
static bool empty_buffer(const struct e2e_peek *k)  { return k->count == 0 && k->active_producers == 0; }
static bool three_consumers(const struct e2e_peek *k) { return k->active_consumers == 3; }

/* ---- Cierre durante actividad ----------------------------------------- */

/* Un consumidor lento y tres productores sin cuota y sin pausas: el buffer
 * se llena y los productores quedan dormidos en not_full. El corte llega
 * así, con mensajes pendientes. Se corre con capacidad 1 y 4. */
static bool close_during_activity(const char *cap) {
    char tag[32];
    snprintf(tag, sizeof(tag), "act%s", cap);
    const char *name = e2e_name("t22", tag);
    const char *c = P(tag, "c1.csv"), *f = P(tag, "f.csv"), *s = P(tag, "s.csv");
    const char *out = P(tag, "fout");
    const char *pl[3] = { P(tag, "p1.csv"), P(tag, "p2.csv"), P(tag, "p3.csv") };
    const char *ids[3] = { "1", "2", "3" };

    CHECK(e2e_run(NULL, "./creator", "--name", name, "--capacity", cap, NULL) == 0);
    pid_t cons = start_consumer(name, "1", "20", c);
    CHECK(e2e_wait_for(name, has_consumer, 5));
    pid_t prod[3];
    for (int i = 0; i < 3; i++)
        prod[i] = start_producer(name, ids[i], "0", pl[i]);

    /* Lleno y con los tres productores registrados: están bloqueados. */
    struct e2e_peek k;
    bool full = false;
    for (int i = 0; i < 500 && !full; i++) {
        full = e2e_peek(name, &k) && k.count == k.capacity && k.active_producers == 3;
        if (!full)
            e2e_sleep_ms(10);
    }
    CHECK(full);

    CHECK(e2e_wait(start_finisher(name, out, s, f), 20) == 0);
    for (int i = 0; i < 3; i++)
        CHECK(e2e_wait(prod[i], 10) == 0);
    CHECK(e2e_wait(cons, 10) == 0);

    /* Había mensajes pendientes en el corte, y aun así nada se perdió:
     * verify exige que todo lo aceptado se haya extraído y que nada se
     * haya insertado después del corte. */
    uint64_t pending = 0;
    CHECK(csv_event_field(f, "CLOSING", COL_OCCUPANCY, &pending));
    CHECK(pending >= 1);
    CHECK(e2e_file_has(out, "CLOSED: recursos liberados"));
    CHECK(e2e_run(NULL, "./verify", pl[0], pl[1], pl[2], c, f, NULL) == 0);
    CHECK(!e2e_instance_exists(name));                  /* recursos liberados */
    CHECK(e2e_file_exists(s));
    return true;
}

static bool test_close_during_activity(void) {
    CHECK(close_during_activity("1"));
    CHECK(close_during_activity("4"));
    return true;
}

/* ---- Cierre en vacío -------------------------------------------------- */

/* Tres consumidores dormidos en not_empty: un productor con cuota ya
 * terminó y lo drenaron todo. El corte los despierta (broadcast, D3) y los
 * tres terminan solos. */
static bool test_close_empty_wakes_consumers(void) {
    const char *name = e2e_name("t22", "empty");
    const char *f = P("empty", "f.csv"), *s = P("empty", "s.csv"), *p = P("empty", "p1.csv");
    const char *out = P("empty", "fout");
    const char *cl[3] = { P("empty", "c1.csv"), P("empty", "c2.csv"), P("empty", "c3.csv") };
    const char *ids[3] = { "1", "2", "3" };

    CHECK(e2e_run(NULL, "./creator", "--name", name, "--capacity", "4", NULL) == 0);
    pid_t cons[3];
    for (int i = 0; i < 3; i++)
        cons[i] = start_consumer(name, ids[i], "0", cl[i]);
    CHECK(e2e_wait_for(name, three_consumers, 5));
    CHECK(e2e_run(NULL, "./producer", "--name", name, "--id", "1", "--mean-ms", "0", "--seed", "1",
                  "--messages", "30", "--log", p, NULL) == 0);
    CHECK(e2e_wait_for(name, empty_buffer, 5));
    e2e_sleep_ms(100);                                  /* ya dormidos en not_empty */

    uint64_t t0 = now_monotonic_ns();
    CHECK(e2e_wait(start_finisher(name, out, s, f), 10) == 0);
    for (int i = 0; i < 3; i++)
        CHECK(e2e_wait(cons[i], 5) == 0);
    /* Despertar es inmediato, no un plazo vencido: holgura 20x sobre lo
     * que tarda una corrida sana. */
    CHECK(now_monotonic_ns() - t0 < 2000000000ull);

    uint64_t pending = 1;
    CHECK(csv_event_field(f, "CLOSING", COL_OCCUPANCY, &pending));
    CHECK(pending == 0);
    CHECK(e2e_run(NULL, "./verify", p, cl[0], cl[1], cl[2], f, NULL) == 0);
    CHECK(!e2e_instance_exists(name));
    return true;
}

/* Sin trabajadores: creator y finisher, nada más. */
static bool test_close_without_workers(void) {
    const char *name = e2e_name("t22", "idle");
    const char *f = P("idle", "f.csv"), *s = P("idle", "s.csv"), *out = P("idle", "fout");
    CHECK(e2e_run(NULL, "./creator", "--name", name, "--capacity", "8", NULL) == 0);
    CHECK(e2e_wait(start_finisher(name, out, s, f), 10) == 0);
    CHECK(!e2e_instance_exists(name));
    CHECK(e2e_file_has(f, ",ALL_LEFT,"));
    CHECK(e2e_file_has(f, ",CLOSED,"));
    CHECK(e2e_file_has(s, "accepted,instance,0,0,msgs"));
    CHECK(e2e_run(NULL, "./verify", f, NULL) == 0);
    /* El nombre quedó libre. */
    CHECK(e2e_run(NULL, "./creator", "--name", name, "--capacity", "8", NULL) == 0);
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* ---- Cierre sin consumidor -------------------------------------------- */

/* Rechazo, y el estado que tiene que quedar intacto para reintentar. */
static bool rejected_stays_open(const char *name, const char *tag, const char *f,
                                uint32_t pending, uint32_t producers) {
    const char *s = P(tag, "srej.csv"), *out = P(tag, "frej");
    struct e2e_peek before, after;
    CHECK(e2e_peek(name, &before));
    CHECK(e2e_wait(start_finisher(name, out, s, f), 10) == 1);
    CHECK(e2e_file_has(out, "no hay consumidores registrados y queda trabajo pendiente"));
    CHECK(e2e_file_has(f, "CLOSE_REJECTED"));
    CHECK(!e2e_file_exists(s));                         /* sin resumen */
    CHECK(e2e_peek(name, &after));
    CHECK(after.state == STATE_OPEN);
    CHECK(after.count == pending && after.active_producers == producers);
    CHECK(after.active_consumers == 0);
    CHECK(after.enqueue_seq == before.enqueue_seq && after.dequeue_seq == before.dequeue_seq);
    return true;
}

/* Un productor activo bloqueado con el buffer lleno y ningún consumidor:
 * se rechaza; sigue vivo y bloqueado; un consumidor nuevo permite cerrar. */
static bool test_no_consumer_active_producer(void) {
    const char *name = e2e_name("t22", "noc1");
    const char *p = P("noc1", "p1.csv"), *c = P("noc1", "c1.csv");
    const char *f = P("noc1", "f.csv"), *s = P("noc1", "s.csv"), *out = P("noc1", "fout");
    const char *frej = P("noc1", "frej.csv");
    CHECK(e2e_run(NULL, "./creator", "--name", name, "--capacity", "4", NULL) == 0);
    pid_t prod = start_producer(name, "1", "0", p);
    struct e2e_peek k;
    bool full = false;
    for (int i = 0; i < 500 && !full; i++) {
        full = e2e_peek(name, &k) && k.count == 4 && k.active_producers == 1;
        if (!full)
            e2e_sleep_ms(10);
    }
    CHECK(full);

    CHECK(rejected_stays_open(name, "noc1", frej, 4, 1));
    CHECK(waitpid(prod, NULL, WNOHANG) == 0);           /* el productor sigue ahí */

    pid_t cons = start_consumer(name, "1", "0", c);
    CHECK(e2e_wait_for(name, has_consumer, 5));
    CHECK(e2e_wait(start_finisher(name, out, s, f), 10) == 0);
    CHECK(e2e_wait(prod, 10) == 0 && e2e_wait(cons, 10) == 0);
    CHECK(e2e_run(NULL, "./verify", p, c, frej, f, NULL) == 0);
    CHECK(!e2e_instance_exists(name));
    return true;
}

/* Datos pendientes y nadie más: el productor ya cumplió su cuota y se fue. */
static bool test_no_consumer_pending_data(void) {
    const char *name = e2e_name("t22", "noc2");
    const char *p = P("noc2", "p1.csv"), *c = P("noc2", "c1.csv");
    const char *f = P("noc2", "f.csv"), *s = P("noc2", "s.csv"), *out = P("noc2", "fout");
    const char *frej = P("noc2", "frej.csv");
    CHECK(e2e_run(NULL, "./creator", "--name", name, "--capacity", "8", NULL) == 0);
    CHECK(e2e_run(NULL, "./producer", "--name", name, "--id", "1", "--mean-ms", "0", "--seed", "1",
                  "--messages", "3", "--log", p, NULL) == 0);

    CHECK(rejected_stays_open(name, "noc2", frej, 3, 0));

    pid_t cons = start_consumer(name, "1", "0", c);
    CHECK(e2e_wait_for(name, has_consumer, 5));
    CHECK(e2e_wait(start_finisher(name, out, s, f), 10) == 0);
    CHECK(e2e_wait(cons, 10) == 0);
    CHECK(e2e_run(NULL, "./verify", p, c, frej, f, NULL) == 0);   /* los 3 */
    CHECK(!e2e_instance_exists(name));
    return true;
}

/* ---- Carreras: incorporaciones durante el cierre ---------------------- */

struct late {
    pid_t       pid;
    const char *out, *log;
    char        role;    /* 'p', 'c', 'f' (finisher) o 'k' (creator) */
};

/* Un rechazo tardío válido: código 1 (no una señal: nada de abortar sobre
 * un mutex destruido) y uno de los mensajes del cierre, según en qué
 * momento llegó: con la puerta cerrada, ya en CLOSED o sin nombre. */
static bool late_rejected_cleanly(const struct late *l) {
    int rc = e2e_wait(l->pid, 10);
    if (rc != 1) {
        fprintf(stderr, "  tardio '%c' termino con %d en vez de 1\n", l->role, rc);
        return false;
    }
    const char *ok[] = { "ya esta cerrandose", "no esta en OPEN", "no existe una instancia",
                         "ya hay un cierre en curso", "ya existe una instancia" };
    char buf[1024] = "";
    FILE *f = fopen(l->out, "r");
    if (f != NULL) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
    }
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        if (strstr(buf, ok[i]) != NULL)
            return true;
    fprintf(stderr, "  tardio '%c' sin mensaje de cierre: %s\n", l->role, buf);
    return false;
}

/* Un consumidor lento drena 64 mensajes pendientes, así el cierre dura
 * cerca de un segundo. Mientras tanto llegan, uno tras otro, productores,
 * consumidores, finalizadores y creadores con el mismo nombre. Al terminar
 * el cierre llega uno más, que ya encuentra el nombre libre. */
static bool test_joins_during_closing(void) {
    const char *name = e2e_name("t22", "join");
    const char *p = P("join", "p1.csv"), *c = P("join", "c1.csv");
    const char *f = P("join", "f.csv"), *s = P("join", "s.csv"), *out = P("join", "fout");
    const char *late_sum = P("join", "lsum");
    CHECK(e2e_run(NULL, "./creator", "--name", name, "--capacity", "64", NULL) == 0);
    /* Primero el productor, para que el buffer quede lleno de verdad. */
    CHECK(e2e_run(NULL, "./producer", "--name", name, "--id", "1", "--mean-ms", "0", "--seed", "1",
                  "--messages", "64", "--log", p, NULL) == 0);
    pid_t cons = start_consumer(name, "1", "15", c);
    CHECK(e2e_wait_for(name, has_consumer, 5));

    pid_t fin = start_finisher(name, out, s, f);
    CHECK(e2e_wait_for(name, not_open, 5));             /* corte hecho, puerta cerrada */

    static struct late late[LATE_MAX];
    const char *roles = "pcfk";
    int n = 0, fin_rc = -2;
    while (n < LATE_MAX) {
        int st;
        if (waitpid(fin, &st, WNOHANG) == fin) {
            e2e_forget_child(fin);                      /* ya cosechado (e2e_util.h) */
            fin_rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            break;
        }
        struct late *l = &late[n];
        char tag[32], id[16];
        snprintf(id, sizeof(id), "%d", 100 + n);
        snprintf(tag, sizeof(tag), "late%d", n);
        l->out = P("join", tag);
        snprintf(tag, sizeof(tag), "late%d.csv", n);
        l->log = P("join", tag);
        l->role = roles[n % 4];
        if (l->role == 'p')
            l->pid = e2e_spawn_all(l->out, "./producer", "--name", name, "--id", id, "--mean-ms", "0",
                                   "--seed", "1", "--messages", "5", "--log", l->log, NULL);
        else if (l->role == 'c')
            l->pid = e2e_spawn_all(l->out, "./consumer", "--name", name, "--id", id, "--mean-ms", "0",
                                   "--seed", "1", "--log", l->log, NULL);
        else if (l->role == 'f')
            l->pid = start_finisher(name, l->out, late_sum, l->log);
        else
            l->pid = e2e_spawn_all(l->out, "./creator", "--name", name, "--capacity", "4", NULL);
        n++;
        e2e_sleep_ms(15);
    }
    if (fin_rc == -2)                                   /* se acabaron los tardíos antes */
        fin_rc = e2e_wait(fin, 20);
    CHECK(fin_rc == 0);
    CHECK(e2e_wait(cons, 10) == 0);
    CHECK(n >= 8);                                      /* de verdad llegaron durante el cierre */
    for (int i = 0; i < n; i++)
        CHECK(late_rejected_cleanly(&late[i]));

    /* Después del cierre: el nombre está libre. */
    const char *after = P("join", "after"), *after_log = P("join", "after.csv");
    CHECK(e2e_run(after, "./consumer", "--name", name, "--id", "999", "--mean-ms", "0",
                  "--seed", "1", "--log", after_log, NULL) == 1);
    CHECK(e2e_file_has(after, "no existe una instancia con ese nombre"));

    /* Ningún tardío alcanzó a insertar ni extraer, y verify acepta todo el
     * conjunto, filas de rechazo de los tardíos incluidas. */
    const char *logs[LATE_MAX + 4];
    int nl = 0;
    logs[nl++] = p;
    logs[nl++] = c;
    logs[nl++] = f;
    logs[nl++] = after_log;
    for (int i = 0; i < n; i++) {
        if (late[i].role == 'k')
            continue;
        CHECK(e2e_file_exists(late[i].log));
        CHECK(!e2e_file_contains(late[i].log, ",PUT,") && !e2e_file_contains(late[i].log, ",GET,"));
        logs[nl++] = late[i].log;
    }
    CHECK(e2e_verify(logs, nl) == 0);
    CHECK(!e2e_instance_exists(name));
    CHECK(!e2e_file_exists(late_sum));                  /* ningún finisher tardío resumió */
    return true;
}

/* ---- Carreras: finalizadores concurrentes ----------------------------- */

/* Cuatro finisher a la vez sobre una instancia con un productor y un
 * consumidor activos, diez rondas. Gana exactamente uno; los demás se
 * rechazan con código 1. Nadie destruye dos veces: un segundo
 * closing_release abortaría (D18) o un shm_unlink tardío borraría la
 * instancia recreada después, y las dos cosas se comprueban. */
static bool test_concurrent_finishers(void) {
    enum { N = 4, ROUNDS = 10 };
    for (int round = 0; round < ROUNDS; round++) {
        char tag[32];
        snprintf(tag, sizeof(tag), "fin%d", round);
        const char *name = e2e_name("t22", tag);
        const char *p = P(tag, "p1.csv"), *c = P(tag, "c1.csv");
        CHECK(e2e_run(NULL, "./creator", "--name", name, "--capacity", "8", NULL) == 0);
        pid_t cons = start_consumer(name, "1", "0.5", c);
        CHECK(e2e_wait_for(name, has_consumer, 5));
        pid_t prod = start_producer(name, "1", "0.5", p);
        e2e_sleep_ms(30);

        const char *out[N], *log[N], *sum[N];
        pid_t fin[N];
        for (int i = 0; i < N; i++) {
            char who[32];
            snprintf(who, sizeof(who), "f%d", i);
            out[i] = P(tag, who);
            snprintf(who, sizeof(who), "f%d.csv", i);
            log[i] = P(tag, who);
            snprintf(who, sizeof(who), "s%d.csv", i);
            sum[i] = P(tag, who);
        }
        for (int i = 0; i < N; i++)
            fin[i] = start_finisher(name, out[i], sum[i], log[i]);

        int winner = -1, losers = 0;
        for (int i = 0; i < N; i++) {
            int rc = e2e_wait(fin[i], 15);
            if (rc == 0 && winner == -1) {
                winner = i;
            } else if (rc == 1) {
                losers++;
                CHECK(!e2e_file_exists(sum[i]));
                CHECK(e2e_file_contains(out[i], "ya hay un cierre en curso") ||
                      e2e_file_contains(out[i], "ya esta cerrandose") ||
                      e2e_file_contains(out[i], "no existe una instancia"));
            } else {
                fprintf(stderr, "  ronda %d: finisher %d termino con %d\n", round, i, rc);
                return false;
            }
        }
        if (winner == -1 || losers != N - 1) {
            fprintf(stderr, "  ronda %d: ganador %d, rechazados %d\n", round, winner, losers);
            return false;
        }
        CHECK(e2e_wait(prod, 10) == 0 && e2e_wait(cons, 10) == 0);
        CHECK(e2e_run(NULL, "./verify", p, c, log[winner], NULL) == 0);
        CHECK(!e2e_instance_exists(name));

        /* Sin doble destrucción: recrear y que la nueva siga ahí un rato
         * después, cuando ya no queda ningún finisher vivo. */
        CHECK(e2e_run(NULL, "./creator", "--name", name, "--capacity", "8", NULL) == 0);
        e2e_sleep_ms(20);
        CHECK(e2e_instance_exists(name));
        CHECK(memory_unlink(name) == 0);
    }
    return true;
}

int main(void) {
    if (access("./creator", X_OK) != 0 || access("./producer", X_OK) != 0 ||
        access("./consumer", X_OK) != 0 || access("./finisher", X_OK) != 0 ||
        access("./verify", X_OK) != 0) {
        fprintf(stderr, "test_closing: faltan los ejecutables "
                        "(correr desde la raiz con make test)\n");
        return 1;
    }
    e2e_install_cleanup();
    alarm(150);   /* tope global; cada proceso hijo tiene además el suyo */
    struct test_case tests[] = {
        {"close_during_activity", test_close_during_activity},
        {"close_empty_wakes_consumers", test_close_empty_wakes_consumers},
        {"close_without_workers", test_close_without_workers},
        {"no_consumer_active_producer", test_no_consumer_active_producer},
        {"no_consumer_pending_data", test_no_consumer_pending_data},
        {"joins_during_closing", test_joins_during_closing},
        {"concurrent_finishers", test_concurrent_finishers},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
