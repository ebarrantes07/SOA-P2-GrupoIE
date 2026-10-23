/* test_participants.c — conexión (D9) y registro de participantes (D8),
 * Issue #12. Las pruebas concurrentes usan procesos reales con fork().
 */
#include "participants.h"
#include "memory.h"
#include "metrics.h"
#include "sync.h"
#include "test_util.h"

#include <stdatomic.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *name_for(const char *tag) {
    static char buf[INSTANCE_NAME_MAX + 1];
    snprintf(buf, sizeof(buf), "/t12_%d_%s", (int)getpid(), tag);
    memory_unlink(buf);
    return buf;
}

/* Crea una instancia y deja el mapeo del creador en *inst. */
static bool make_instance(const char *name, struct instance **inst, size_t *len) {
    return memory_create(name, 4, inst, len) == MEM_OK;
}

static void done(const char *name, struct instance *inst, size_t len) {
    memory_unmap(inst, len);
    memory_unlink(name);
}

static bool test_register_basic(void) {
    const char *name = name_for("basic");
    struct instance *inst;
    size_t len;
    CHECK(make_instance(name, &inst, &len));

    int slot = -1;
    CHECK(participant_register(inst, ROLE_PRODUCER, 7, &slot) == REG_OK);
    CHECK(slot == 0);
    CHECK(inst->producers.used == 1);
    CHECK(inst->active_producers == 1 && inst->active_consumers == 0);
    struct participant_slot *s = &inst->producers.slots[0];
    CHECK(s->id == 7 && s->state == SLOT_ACTIVE && s->pid == (int32_t)getpid());
    CHECK(s->joined_ns > 0);

    uint64_t first = inst->metrics.first_start_ns;
    CHECK(first == s->joined_ns);       /* el primer trabajador fija el inicio (D11) */
    CHECK(participant_register(inst, ROLE_CONSUMER, 7, &slot) == REG_OK);   /* otro rol */
    CHECK(slot == 0);
    CHECK(inst->active_consumers == 1);
    CHECK(inst->metrics.first_start_ns == first);   /* no lo mueve el segundo */

    done(name, inst, len);
    return true;
}

static bool test_duplicate_rejected_table_intact(void) {
    const char *name = name_for("dup");
    struct instance *inst;
    size_t len;
    CHECK(make_instance(name, &inst, &len));
    int slot;
    CHECK(participant_register(inst, ROLE_CONSUMER, 1, &slot) == REG_OK);
    CHECK(participant_register(inst, ROLE_CONSUMER, 2, &slot) == REG_OK);

    struct participant_table before = inst->consumers;
    uint32_t active = inst->active_consumers;
    int untouched = 99;
    CHECK(participant_register(inst, ROLE_CONSUMER, 2, &untouched) == REG_DUPLICATE_ID);
    CHECK(memcmp(&before, &inst->consumers, sizeof(before)) == 0);
    CHECK(inst->active_consumers == active);
    CHECK(untouched == 99);              /* un rechazo no escribe *slot */

    done(name, inst, len);
    return true;
}

static bool test_duplicate_after_leave(void) {
    const char *name = name_for("left");
    struct instance *inst;
    size_t len;
    CHECK(make_instance(name, &inst, &len));
    int slot;
    CHECK(participant_register(inst, ROLE_PRODUCER, 5, &slot) == REG_OK);
    participant_leave(inst, ROLE_PRODUCER, slot, NULL);
    CHECK(inst->active_producers == 0);
    /* De por vida: el id sigue reservado aunque su dueño ya se fue (D8). */
    CHECK(participant_register(inst, ROLE_PRODUCER, 5, &slot) == REG_DUPLICATE_ID);
    done(name, inst, len);
    return true;
}

static bool test_not_open_rejected(void) {
    const char *name = name_for("closing");
    struct instance *inst;
    size_t len;
    CHECK(make_instance(name, &inst, &len));
    int slot;
    CHECK(participant_register(inst, ROLE_PRODUCER, 1, &slot) == REG_OK);

    sync_lock(inst);
    inst->state = STATE_CLOSING;
    sync_unlock(inst);

    struct participant_table before = inst->producers;
    CHECK(participant_register(inst, ROLE_PRODUCER, 2, &slot) == REG_NOT_OPEN);
    CHECK(participant_register(inst, ROLE_CONSUMER, 3, &slot) == REG_NOT_OPEN);
    CHECK(memcmp(&before, &inst->producers, sizeof(before)) == 0);
    CHECK(inst->consumers.used == 0 && inst->active_consumers == 0);
    done(name, inst, len);
    return true;
}

static bool test_table_capacity(void) {
    const char *name = name_for("full");
    struct instance *inst;
    size_t len;
    CHECK(make_instance(name, &inst, &len));
    int slot;
    for (uint32_t id = 1; id <= MAX_PARTICIPANTS_PER_ROLE; id++) {
        CHECK(participant_register(inst, ROLE_PRODUCER, id, &slot) == REG_OK);
        CHECK(participant_register(inst, ROLE_CONSUMER, id, &slot) == REG_OK);
    }
    CHECK(inst->active_producers == MAX_PARTICIPANTS_PER_ROLE);
    CHECK(inst->active_consumers == MAX_PARTICIPANTS_PER_ROLE);

    struct participant_table before = inst->producers;
    CHECK(participant_register(inst, ROLE_PRODUCER, 1000, &slot) == REG_TABLE_FULL);
    CHECK(memcmp(&before, &inst->producers, sizeof(before)) == 0);
    /* Un duplicado con la tabla llena se reporta como duplicado. */
    CHECK(participant_register(inst, ROLE_PRODUCER, 1, &slot) == REG_DUPLICATE_ID);
    done(name, inst, len);
    return true;
}

static bool test_leave_copies_stats(void) {
    const char *name = name_for("stats");
    struct instance *inst;
    size_t len;
    CHECK(make_instance(name, &inst, &len));
    int slot;
    CHECK(participant_register(inst, ROLE_CONSUMER, 9, &slot) == REG_OK);
    struct participant_stats st = { .ops = 100, .wait_ns = 2000, .wait_count = 3, .lock_ns = 400 };
    participant_leave(inst, ROLE_CONSUMER, slot, &st);

    struct participant_slot *s = &inst->consumers.slots[slot];
    CHECK(s->state == SLOT_LEFT);
    CHECK(s->ops == 100 && s->wait_ns == 2000 && s->wait_count == 3 && s->lock_ns == 400);
    CHECK(s->left_ns >= s->joined_ns);
    CHECK(inst->active_consumers == 0);
    CHECK(inst->consumers.used == 1);      /* la casilla no se libera */
    done(name, inst, len);
    return true;
}

static bool test_attach_detach_refs(void) {
    const char *name = name_for("refs");
    struct instance *creator;
    size_t clen;
    CHECK(make_instance(name, &creator, &clen));

    struct instance *a, *b;
    size_t la, lb;
    CHECK(instance_attach(name, &a, &la) == ATTACH_OK);
    CHECK(atomic_load(&creator->refs) == 1);
    CHECK(instance_attach(name, &b, &lb) == ATTACH_OK);
    CHECK(atomic_load(&creator->refs) == 2);
    instance_detach(a, la);
    CHECK(atomic_load(&creator->refs) == 1);
    instance_detach(b, lb);
    CHECK(atomic_load(&creator->refs) == 0);
    done(name, creator, clen);
    return true;
}

static bool test_attach_errors(void) {
    struct instance *inst;
    size_t len;
    CHECK(instance_attach("sin_barra", &inst, &len) == ATTACH_INVALID_NAME);
    CHECK(instance_attach(name_for("nadie"), &inst, &len) == ATTACH_NOT_FOUND);

    /* Puerta cerrada: no entra y no cuenta como conexión (D9). */
    const char *name = name_for("gate");
    struct instance *creator;
    size_t clen;
    CHECK(make_instance(name, &creator, &clen));
    struct instance *a;
    size_t la;
    CHECK(instance_attach(name, &a, &la) == ATTACH_OK);
    atomic_fetch_or(&creator->refs, REFS_GATE_CLOSED);
    CHECK(instance_attach(name, &inst, &len) == ATTACH_CLOSING);
    CHECK((atomic_load(&creator->refs) & REFS_COUNT_MASK) == 1);
    instance_detach(a, la);              /* quien ya estaba adentro sí sale */
    CHECK(atomic_load(&creator->refs) == REFS_GATE_CLOSED);
    done(name, creator, clen);
    return true;
}

/* 32 productores y 32 consumidores, cada uno un proceso, registrándose a la
 * vez con ids únicos. Todos entran y la tabla termina coherente. */
static bool test_concurrent_registration(void) {
    enum { PER_ROLE = 32 };
    const char *name = name_for("conc");
    struct instance *inst;
    size_t len;
    CHECK(make_instance(name, &inst, &len));

    for (int c = 0; c < 2 * PER_ROLE; c++) {
        pid_t pid = fork();
        CHECK(pid != -1);
        if (pid == 0) {
            alarm(15);
            struct instance *mine;
            size_t ml;
            if (instance_attach(name, &mine, &ml) != ATTACH_OK)
                _exit(2);
            enum role role = c < PER_ROLE ? ROLE_PRODUCER : ROLE_CONSUMER;
            int slot;
            enum register_result r = participant_register(mine, role, (uint32_t)(c % PER_ROLE) + 1, &slot);
            instance_detach(mine, ml);
            _exit(r == REG_OK ? 0 : 1);
        }
    }
    bool all_ok = true;
    for (int c = 0; c < 2 * PER_ROLE; c++) {
        int st;
        if (wait(&st) == -1 || !WIFEXITED(st) || WEXITSTATUS(st) != 0)
            all_ok = false;
    }
    CHECK(all_ok);
    CHECK(inst->producers.used == PER_ROLE && inst->consumers.used == PER_ROLE);
    CHECK(inst->active_producers == PER_ROLE && inst->active_consumers == PER_ROLE);
    CHECK(atomic_load(&inst->refs) == 0);

    /* Cada id de 1 a 32 aparece exactamente una vez por rol. */
    int seen_p[PER_ROLE + 1] = {0}, seen_c[PER_ROLE + 1] = {0};
    for (int i = 0; i < PER_ROLE; i++) {
        uint32_t p = inst->producers.slots[i].id, q = inst->consumers.slots[i].id;
        CHECK(p >= 1 && p <= PER_ROLE && q >= 1 && q <= PER_ROLE);
        seen_p[p]++;
        seen_c[q]++;
    }
    for (int id = 1; id <= PER_ROLE; id++)
        CHECK(seen_p[id] == 1 && seen_c[id] == 1);
    done(name, inst, len);
    return true;
}

/* 16 procesos intentan registrar los mismos 64 ids a la vez. Una largada
 * por pipe los suelta juntos (crear procesos tarda lo suficiente para que,
 * sin ella, casi nunca coincidan). Cada id tiene que quedar con un solo
 * dueño: sin el mutex aparecen ids repetidos o casillas pisadas. */
static bool test_concurrent_duplicate_race(void) {
    enum { RACERS = 16, IDS = MAX_PARTICIPANTS_PER_ROLE };
    const char *name = name_for("race");
    struct instance *inst;
    size_t len;
    CHECK(make_instance(name, &inst, &len));
    int start[2];
    CHECK(pipe(start) == 0);

    for (int c = 0; c < RACERS; c++) {
        pid_t pid = fork();
        CHECK(pid != -1);
        if (pid == 0) {
            alarm(15);
            close(start[1]);
            struct instance *mine;
            size_t ml;
            if (instance_attach(name, &mine, &ml) != ATTACH_OK)
                _exit(255);
            char go;
            if (read(start[0], &go, 1) != 1)        /* bloquea hasta la largada */
                _exit(255);
            int won = 0;
            for (uint32_t id = 1; id <= IDS; id++) {
                int slot;
                enum register_result r = participant_register(mine, ROLE_CONSUMER, id, &slot);
                if (r == REG_OK)
                    won++;
                else if (r != REG_DUPLICATE_ID)
                    _exit(254);
            }
            instance_detach(mine, ml);
            _exit(won);
        }
    }
    close(start[0]);
    struct timespec settle = { 0, 200 * 1000 * 1000 };
    nanosleep(&settle, NULL);                        /* que todos lleguen a la largada */
    char go[RACERS];
    memset(go, 'g', sizeof(go));
    CHECK(write(start[1], go, sizeof(go)) == (ssize_t)sizeof(go));
    close(start[1]);

    int total = 0;
    bool clean = true;
    for (int c = 0; c < RACERS; c++) {
        int st;
        if (wait(&st) == -1 || !WIFEXITED(st) || WEXITSTATUS(st) > IDS)
            clean = false;
        else
            total += WEXITSTATUS(st);
    }
    CHECK(clean);
    CHECK(total == IDS);                             /* cada id lo ganó uno solo */
    CHECK(inst->consumers.used == IDS && inst->active_consumers == IDS);
    int seen[IDS + 1] = {0};
    for (int k = 0; k < IDS; k++) {
        uint32_t id = inst->consumers.slots[k].id;
        CHECK(id >= 1 && id <= IDS);
        seen[id]++;
    }
    for (int id = 1; id <= IDS; id++)
        CHECK(seen[id] == 1);
    done(name, inst, len);
    return true;
}

/* instance_detach despierta a quien espera en all_left (el finisher, D9). */
static bool test_detach_wakes_waiter(void) {
    const char *name = name_for("wake");
    struct instance *inst;
    size_t len;
    CHECK(make_instance(name, &inst, &len));
    struct instance *me, *worker;
    size_t ml, wl;
    CHECK(instance_attach(name, &me, &ml) == ATTACH_OK);       /* "finisher" */
    /* La conexión del trabajador se toma antes del fork y el hijo la
     * hereda: así refs ya vale 2 cuando el padre evalúa el predicado y la
     * prueba siempre ejercita el despertar. */
    CHECK(instance_attach(name, &worker, &wl) == ATTACH_OK);

    pid_t pid = fork();
    CHECK(pid != -1);
    if (pid == 0) {
        struct timespec d = { 0, 100 * 1000 * 1000 };       /* que el padre ya duerma */
        nanosleep(&d, NULL);
        instance_detach(worker, wl);
        _exit(0);
    }
    memory_unmap(worker, wl);            /* el padre suelta su copia del mapeo, no la referencia */

    alarm(10);                           /* si la señal se perdiera, falla en vez de colgarse */
    sync_lock(me);
    while ((atomic_load(&me->refs) & REFS_COUNT_MASK) > 1)   /* predicado de D9 */
        pthread_cond_wait(&me->all_left, &me->mutex);
    sync_unlock(me);
    alarm(0);

    int st;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK((atomic_load(&me->refs) & REFS_COUNT_MASK) == 1);
    instance_detach(me, ml);
    done(name, inst, len);
    return true;
}

/* ---- Corte a CLOSING (D10, #16) --------------------------------------- */

/* La precondicion de D10 tiene cuatro combinaciones y la expresion es
 * facil de leer mal, asi que van las cuatro. */
static bool test_closing_precondition_with_consumer(void) {
    const char *name = name_for("cl_ok");
    struct instance *inst;
    size_t len;
    CHECK(make_instance(name, &inst, &len));
    int slot;
    CHECK(participant_register(inst, ROLE_CONSUMER, 1, &slot) == REG_OK);

    struct close_snapshot snap;
    CHECK(closing_begin(inst, &snap) == CLOSE_OK);
    CHECK(inst->state == STATE_CLOSING);
    CHECK(inst->metrics.closing_start_ns > 0);
    CHECK(snap.closing_ns == inst->metrics.closing_start_ns);

    done(name, inst, len);
    return true;
}

/* Sin nadie y sin pendientes se puede cerrar: no queda nada por drenar. */
static bool test_closing_precondition_with_nothing_pending(void) {
    const char *name = name_for("cl_emp");
    struct instance *inst;
    size_t len;
    CHECK(make_instance(name, &inst, &len));

    CHECK(inst->active_producers == 0 && inst->active_consumers == 0);
    CHECK(inst->count == 0);
    CHECK(closing_begin(inst, NULL) == CLOSE_OK);      /* snapshot opcional */
    CHECK(inst->state == STATE_CLOSING);

    done(name, inst, len);
    return true;
}

/* Un productor activo y ningun consumidor: nadie sacaria lo que inserte. */
static bool test_closing_rejects_producer_without_consumer(void) {
    const char *name = name_for("cl_np");
    struct instance *inst;
    size_t len;
    CHECK(make_instance(name, &inst, &len));
    int slot;
    CHECK(participant_register(inst, ROLE_PRODUCER, 1, &slot) == REG_OK);

    struct close_snapshot snap;
    memset(&snap, 0xAA, sizeof(snap));                 /* basura previa */
    CHECK(closing_begin(inst, &snap) == CLOSE_NO_CONSUMER);

    /* Un rechazo no toca nada: la instancia sigue utilizable. */
    CHECK(inst->state == STATE_OPEN);
    CHECK(inst->metrics.closing_start_ns == 0);
    CHECK((atomic_load(&inst->refs) & REFS_GATE_CLOSED) == 0);
    CHECK(snap.accepted == 0 && snap.extracted == 0 && snap.pending == 0);

    /* Y se puede reintentar tras levantar un consumidor (D10). */
    CHECK(participant_register(inst, ROLE_CONSUMER, 1, &slot) == REG_OK);
    CHECK(closing_begin(inst, NULL) == CLOSE_OK);

    done(name, inst, len);
    return true;
}

/* Sin participantes pero con mensajes en el buffer: tampoco se cierra,
 * porque esos mensajes no los sacaria nadie. */
static bool test_closing_rejects_pending_without_consumer(void) {
    const char *name = name_for("cl_pend");
    struct instance *inst;
    size_t len;
    CHECK(make_instance(name, &inst, &len));

    struct message m = {0};
    m.producer_id = 1;
    m.sequence = 1;
    CHECK(sync_put(inst, &m, NULL) == PUT_OK);         /* no exige registro */
    CHECK(inst->count == 1);
    CHECK(inst->active_producers == 0);

    CHECK(closing_begin(inst, NULL) == CLOSE_NO_CONSUMER);
    CHECK(inst->state == STATE_OPEN);

    done(name, inst, len);
    return true;
}

/* El finalizador unico sin bandera extra: la transicion es la reserva. */
static bool test_closing_second_call_rejected(void) {
    const char *name = name_for("cl_2nd");
    struct instance *inst;
    size_t len;
    CHECK(make_instance(name, &inst, &len));

    struct close_snapshot first, second;
    CHECK(closing_begin(inst, &first) == CLOSE_OK);
    memset(&second, 0xAA, sizeof(second));
    CHECK(closing_begin(inst, &second) == CLOSE_ALREADY_CLOSING);
    CHECK(second.accepted == 0 && second.pending == 0);   /* quedo en cero */
    CHECK(inst->metrics.closing_start_ns == first.closing_ns);  /* no se piso */

    done(name, inst, len);
    return true;
}

/* El corte cierra la puerta: nadie mas se conecta (D9). */
static bool test_closing_closes_the_gate(void) {
    const char *name = name_for("cl_gate");
    struct instance *inst;
    size_t len;
    CHECK(make_instance(name, &inst, &len));

    struct instance *otro;
    size_t ol;
    CHECK(instance_attach(name, &otro, &ol) == ATTACH_OK);   /* antes: entra */
    instance_detach(otro, ol);

    CHECK(closing_begin(inst, NULL) == CLOSE_OK);
    CHECK(instance_attach(name, &otro, &ol) == ATTACH_CLOSING);   /* despues: no */

    done(name, inst, len);
    return true;
}

/* El punto de este slice: el snapshot describe el corte, y sigue
 * describiendolo aunque los consumidores sigan drenando despues. */
static bool test_closing_snapshot_describes_the_cut(void) {
    const char *name = name_for("cl_snap");
    struct instance *inst;
    size_t len;
    CHECK(make_instance(name, &inst, &len));
    int slot;
    CHECK(participant_register(inst, ROLE_CONSUMER, 1, &slot) == REG_OK);

    for (uint64_t i = 1; i <= 4; i++) {        /* capacidad 4: se llena */
        struct message m = {0};
        m.producer_id = 1;
        m.sequence = i;
        CHECK(sync_put(inst, &m, NULL) == PUT_OK);
    }
    struct message out;
    CHECK(sync_get(inst, &out, NULL) == GET_OK);
    CHECK(sync_get(inst, &out, NULL) == GET_OK);

    struct close_snapshot snap;
    CHECK(closing_begin(inst, &snap) == CLOSE_OK);
    CHECK(snap.accepted == 4);
    CHECK(snap.extracted == 2);
    CHECK(snap.pending == 2);
    CHECK(snap.accepted == snap.extracted + snap.pending);   /* lo que audita #7 */

    /* El consumidor sigue drenando en CLOSING (D10) y el segmento cambia,
     * pero el snapshot es una copia: no se mueve. */
    CHECK(sync_get(inst, &out, NULL) == GET_OK);
    CHECK(inst->dequeue_seq == 3 && inst->count == 1);
    CHECK(snap.extracted == 2 && snap.pending == 2);

    done(name, inst, len);
    return true;
}

static bool test_close_result_str(void) {
    const enum close_result rs[] = {CLOSE_OK, CLOSE_NO_CONSUMER,
                                    CLOSE_ALREADY_CLOSING};
    for (size_t i = 0; i < sizeof(rs) / sizeof(rs[0]); i++) {
        const char *m = close_result_str(rs[i]);
        CHECK(m != NULL && m[0] != '\0');
        CHECK(strstr(m, "desconocido") == NULL);
    }
    return true;
}

int main(void) {
    struct test_case tests[] = {
        {"register_basic", test_register_basic},
        {"duplicate_rejected_table_intact", test_duplicate_rejected_table_intact},
        {"duplicate_after_leave", test_duplicate_after_leave},
        {"not_open_rejected", test_not_open_rejected},
        {"table_capacity", test_table_capacity},
        {"leave_copies_stats", test_leave_copies_stats},
        {"attach_detach_refs", test_attach_detach_refs},
        {"attach_errors", test_attach_errors},
        {"concurrent_registration", test_concurrent_registration},
        {"concurrent_duplicate_race", test_concurrent_duplicate_race},
        {"detach_wakes_waiter", test_detach_wakes_waiter},
        {"closing_precondition_with_consumer", test_closing_precondition_with_consumer},
        {"closing_precondition_with_nothing_pending", test_closing_precondition_with_nothing_pending},
        {"closing_rejects_producer_without_consumer", test_closing_rejects_producer_without_consumer},
        {"closing_rejects_pending_without_consumer", test_closing_rejects_pending_without_consumer},
        {"closing_second_call_rejected", test_closing_second_call_rejected},
        {"closing_closes_the_gate", test_closing_closes_the_gate},
        {"closing_snapshot_describes_the_cut", test_closing_snapshot_describes_the_cut},
        {"close_result_str", test_close_result_str},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
