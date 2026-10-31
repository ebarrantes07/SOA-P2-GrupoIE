/* producer.c — genera mensajes con payload verificable y los inserta.
 *
 *   ./producer --name /soa_p2_g1 --id 1 --mean-ms 5 --seed 7 --log results/p1.csv
 *   ./producer ... --messages 1000        # cuota; sin ella, hasta el cierre
 *
 * Se conecta y se registra (D8, D9), y repite: pausa exponencial
 * interrumpible por el cierre (D20), arma el mensaje (D1, D22) e inserta
 * bloqueandose con el buffer lleno (D3). Al notar el corte se retira sin
 * insertar nada mas (D10). Cada evento va a su CSV (D12).
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cli.h"
#include "clock.h"
#include "logger.h"
#include "memory.h"
#include "metrics.h"
#include "participants.h"
#include "payload.h"
#include "rng.h"
#include "sync.h"

static const char *attach_detail(enum attach_result r) {
    switch (r) {
    case ATTACH_NOT_FOUND: return "not_found";
    case ATTACH_CLOSING:   return "closing";
    default:               return attach_result_str(r);
    }
}

static const char *register_detail(enum register_result r) {
    switch (r) {
    case REG_DUPLICATE_ID: return "duplicate_id";
    case REG_NOT_OPEN:     return "not_open";
    case REG_TABLE_FULL:   return "table_full";
    case REG_OK:           break;
    }
    return "ok";
}

/* Escribe una fila y recuerda el primer error, sin cortar la corrida: el
 * productor tiene que retirarse y soltar su referencia aunque el disco
 * falle, o el finisher esperaria para siempre. Se reporta al final. */
static void log_row(struct csv_logger *lg, const struct csv_record *rec, int *first_err) {
    int rc = csv_logger_write(lg, rec);
    if (rc != 0 && *first_err == 0)
        *first_err = rc;
}

int main(int argc, char **argv) {
    struct cli_options opt;
    char err[256];
    switch (cli_parse(CLI_PRODUCER, argc, argv, &opt, err, sizeof(err))) {
    case CLI_HELP:
        cli_usage(CLI_PRODUCER, stdout);
        return EXIT_SUCCESS;
    case CLI_ERROR:
        fprintf(stderr, "producer: %s\n", err);
        cli_usage(CLI_PRODUCER, stderr);
        return EXIT_FAILURE;
    case CLI_OK:
        break;
    }

    /* El CSV se abre primero, asi un rechazo al conectarse o registrarse
     * tambien queda registrado (mismo orden que el consumer, D23). */
    struct csv_logger lg;
    int rc = csv_logger_open(&lg, opt.log_path, CSV_PRODUCER, opt.id);
    if (rc != 0) {
        fprintf(stderr, "producer: no se pudo abrir %s: %s\n", opt.log_path, csv_logger_open_str(rc));
        return EXIT_FAILURE;
    }

    struct rng rng;
    if (rng_init(&rng, opt.seed) != 0) {
        /* Inalcanzable: la CLI exige --seed en 1..UINT32_MAX. Si alguna vez
         * deja de exigirlo, rng_init no toca la estructura y seguir usaria
         * memoria sin inicializar: es error de programacion, aborta (D18). */
        fprintf(stderr, "producer: semilla invalida (%" PRIu32 ")\n", opt.seed);
        csv_logger_close(&lg);
        abort();
    }

    struct csv_record rec;
    int write_err = 0;

    struct instance *inst;
    size_t len;
    enum attach_result ar = instance_attach(opt.name, &inst, &len);
    if (ar != ATTACH_OK) {
        csv_record_event(&rec, CSV_JOIN_REJECTED, now_monotonic_ns(), attach_detail(ar));
        log_row(&lg, &rec, &write_err);
        csv_logger_close(&lg);
        if (ar == ATTACH_SYS_ERROR)
            fprintf(stderr, "producer: %s: %s\n", attach_result_str(ar), strerror(errno));
        else
            fprintf(stderr, "producer: %s: %s\n", opt.name, attach_result_str(ar));
        return EXIT_FAILURE;
    }

    int slot;
    enum register_result rr = participant_register(inst, ROLE_PRODUCER, opt.id, &slot);
    if (rr != REG_OK) {
        csv_record_event(&rec, CSV_JOIN_REJECTED, now_monotonic_ns(), register_detail(rr));
        log_row(&lg, &rec, &write_err);
        instance_detach(inst, len);
        csv_logger_close(&lg);
        fprintf(stderr, "producer %" PRIu32 ": %s\n", opt.id, register_result_str(rr));
        return EXIT_FAILURE;
    }

    char pid[16];
    snprintf(pid, sizeof(pid), "%ld", (long)getpid());
    csv_record_event(&rec, CSV_JOIN, now_monotonic_ns(), pid);
    log_row(&lg, &rec, &write_err);

    struct participant_stats stats = {0};
    uint64_t sequence = 0;        /* por productor, el primero es 1 (D1) */
    uint64_t rejected = 0;
    const char *leave_detail = "quota_done";

    for (;;) {
        if (opt.has_messages && stats.ops == opt.messages)
            break;                /* cuota cumplida: leave_detail ya es quota_done */

        /* Pausa ANTES de cada intento, interrumpible por el corte (D20).
         * Con --mean-ms 0 no se llama al muestreo ni se duerme (#5), asi
         * que el corte se nota recien en sync_put. */
        if (opt.mean_ms > 0.0 &&
            sync_pause_ms(inst, rng_exponential_ms(&rng, opt.mean_ms)) == PAUSE_INTERRUPTED) {
            /* Se retira sin insertar nada mas: ni siquiera intenta, asi no
             * ensucia el CSV con un PUT_REJECTED que no aporta. */
            leave_detail = "closing";
            break;
        }

        /* En cero completo y no campo por campo: `reserved` viaja a la
         * memoria compartida y D13 lo quiere siempre en 0. */
        struct message msg;
        memset(&msg, 0, sizeof(msg));
        msg.producer_id = opt.id;
        msg.sequence = ++sequence;
        payload_build(&msg.payload, opt.id, msg.sequence);

        struct op_snapshot snap;
        if (sync_put(inst, &msg, &snap) == PUT_REJECTED_CLOSING) {
            /* La espera por espacio ocurrio aunque el intento se rechazara
             * (D11): se acumula igual o el resumen la perderia. */
            stats.wait_ns += snap.wait_ns;
            stats.wait_count += snap.wait_count;
            stats.lock_ns += snap.lock_ns;
            rejected++;
            csv_record_event(&rec, CSV_PUT_REJECTED, now_monotonic_ns(), "closing");
            rec.producer_id = msg.producer_id;
            rec.sequence = msg.sequence;
            log_row(&lg, &rec, &write_err);
            leave_detail = "closing";
            break;
        }

        stats.ops++;
        stats.wait_ns += snap.wait_ns;
        stats.wait_count += snap.wait_count;
        stats.lock_ns += snap.lock_ns;

        /* Ya fuera del mutex: se escribe lo capturado en el snapshot. */
        csv_record_put(&rec, &msg, &snap);
        log_row(&lg, &rec, &write_err);
    }

    participant_leave(inst, ROLE_PRODUCER, slot, &stats);
    csv_record_event(&rec, CSV_LEAVE, now_monotonic_ns(), leave_detail);
    log_row(&lg, &rec, &write_err);
    instance_detach(inst, len);

    int close_err = csv_logger_close(&lg);
    if (write_err == 0)
        write_err = close_err;

    printf("producer %" PRIu32 ": %" PRIu64 " mensajes aceptados", opt.id, stats.ops);
    if (rejected > 0)
        printf(", %" PRIu64 " rechazado por el cierre", rejected);
    printf(" (%s)\n", leave_detail);

    if (write_err != 0) {
        fprintf(stderr, "producer %" PRIu32 ": error escribiendo %s: %s\n",
                opt.id, opt.log_path, strerror(-write_err));
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
