/* consumer.c — extrae mensajes FIFO y simula su procesamiento.
 *
 *   ./consumer --name /soa_p2_g1 --id 1 --mean-ms 10 --seed 11 --log results/c1.csv
 *
 * Se conecta y se registra (D8, D9), extrae bloqueándose con el buffer
 * vacío (D3), verifica cada payload (D1, D22) y simula el procesamiento
 * fuera de la región crítica. Sale solo cuando ya no pueden llegar datos:
 * instancia en CLOSING y buffer vacío (D10). Cada evento va a su CSV (D12).
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

/* El procesamiento simulado NO se interrumpe con el cierre: el consumidor
 * tiene que terminar lo que ya retiró (D10). Se reanuda si una señal corta
 * el nanosleep. */
static void simulate_processing(double ms) {
    uint64_t ns = (uint64_t)(ms * 1000000.0);
    struct timespec req = {
        .tv_sec = (time_t)(ns / 1000000000ull),
        .tv_nsec = (long)(ns % 1000000000ull),
    };
    struct timespec rem;
    while (nanosleep(&req, &rem) == -1 && errno == EINTR)
        req = rem;
}

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
 * consumidor tiene que seguir drenando y retirarse aunque el disco falle,
 * o el finisher esperaría para siempre. El error se reporta al final. */
static void log_row(struct csv_logger *lg, const struct csv_record *rec, int *first_err) {
    int rc = csv_logger_write(lg, rec);
    if (rc != 0 && *first_err == 0)
        *first_err = rc;
}

int main(int argc, char **argv) {
    struct cli_options opt;
    char err[256];
    switch (cli_parse(CLI_CONSUMER, argc, argv, &opt, err, sizeof(err))) {
    case CLI_HELP:
        cli_usage(CLI_CONSUMER, stdout);
        return EXIT_SUCCESS;
    case CLI_ERROR:
        fprintf(stderr, "consumer: %s\n", err);
        cli_usage(CLI_CONSUMER, stderr);
        return EXIT_FAILURE;
    case CLI_OK:
        break;
    }

    /* El CSV se abre primero para que también quede registro de un
     * rechazo al conectarse o registrarse. */
    struct csv_logger lg;
    int rc = csv_logger_open(&lg, opt.log_path, CSV_CONSUMER, opt.id);
    if (rc != 0) {
        fprintf(stderr, "consumer: no se pudo abrir %s: %s\n", opt.log_path, strerror(-rc));
        return EXIT_FAILURE;
    }

    struct rng rng;
    rng_init(&rng, opt.seed);       /* la CLI ya garantizó seed != 0 */

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
            fprintf(stderr, "consumer: %s: %s\n", attach_result_str(ar), strerror(errno));
        else
            fprintf(stderr, "consumer: %s: %s\n", opt.name, attach_result_str(ar));
        return EXIT_FAILURE;
    }

    int slot;
    enum register_result rr = participant_register(inst, ROLE_CONSUMER, opt.id, &slot);
    if (rr != REG_OK) {
        csv_record_event(&rec, CSV_JOIN_REJECTED, now_monotonic_ns(), register_detail(rr));
        log_row(&lg, &rec, &write_err);
        instance_detach(inst, len);
        csv_logger_close(&lg);
        fprintf(stderr, "consumer %" PRIu32 ": %s\n", opt.id, register_result_str(rr));
        return EXIT_FAILURE;
    }

    char pid[16];
    snprintf(pid, sizeof(pid), "%ld", (long)getpid());
    csv_record_event(&rec, CSV_JOIN, now_monotonic_ns(), pid);
    log_row(&lg, &rec, &write_err);

    struct participant_stats stats = {0};
    uint64_t corrupted = 0;

    for (;;) {
        struct message msg;
        struct op_snapshot snap;
        if (sync_get(inst, &msg, &snap) == GET_DRAINED) {
            /* CLOSING y buffer vacío: ya no pueden llegar datos. La espera
             * final también se acumula (D11 la declara combinada). */
            stats.wait_ns += snap.wait_ns;
            stats.wait_count += snap.wait_count;
            stats.lock_ns += snap.lock_ns;
            csv_record_event(&rec, CSV_DRAINED, now_monotonic_ns(), NULL);
            rec.occupancy = 0;
            log_row(&lg, &rec, &write_err);
            break;
        }

        /* Ya fuera del mutex: verificar, registrar y procesar. */
        bool ok = payload_verify(&msg);
        if (!ok)
            corrupted++;
        stats.ops++;
        stats.wait_ns += snap.wait_ns;
        stats.wait_count += snap.wait_count;
        stats.lock_ns += snap.lock_ns;

        csv_record_get(&rec, &msg, &snap, ok);
        log_row(&lg, &rec, &write_err);

        if (opt.mean_ms > 0.0)       /* --mean-ms 0 no llama al muestreo (#5) */
            simulate_processing(rng_exponential_ms(&rng, opt.mean_ms));

        uint64_t dseq = rec.dequeue_seq;
        csv_record_event(&rec, CSV_PROCESSED, now_monotonic_ns(), NULL);
        rec.producer_id = msg.producer_id;
        rec.sequence = msg.sequence;
        rec.dequeue_seq = dseq;
        log_row(&lg, &rec, &write_err);
    }

    participant_leave(inst, ROLE_CONSUMER, slot, &stats);
    csv_record_event(&rec, CSV_LEAVE, now_monotonic_ns(), "drained");
    log_row(&lg, &rec, &write_err);
    instance_detach(inst, len);

    int close_err = csv_logger_close(&lg);
    if (write_err == 0)
        write_err = close_err;

    printf("consumer %" PRIu32 ": %" PRIu64 " mensajes extraidos", opt.id, stats.ops);
    if (corrupted > 0)
        printf(", %" PRIu64 " con payload alterado", corrupted);
    printf("\n");

    if (write_err != 0) {
        fprintf(stderr, "consumer %" PRIu32 ": error escribiendo %s: %s\n",
                opt.id, opt.log_path, strerror(-write_err));
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
