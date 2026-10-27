/* finisher.c — conduce el cierre ordenado de una instancia.
 *
 *   ./finisher --name /soa_p2_g1 --summary results/resumen.csv [--log results/fin.csv]
 *
 * Orden de D10: conectarse, cortar a CLOSING (closing_begin, con el
 * snapshot del corte), esperar a que todos los participantes se retiren
 * (closing_wait_all_left), liberar primitivas y nombre (closing_release),
 * escribir el resumen y desmapear. Si la precondición falla o ya hay otro
 * cierre en curso, no toca nada y sale con 1. Los eventos van a la consola
 * y, con --log, también a su CSV (D12).
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "clock.h"
#include "logger.h"
#include "memory.h"
#include "participants.h"

/* Bitácora del finisher: siempre una línea en la consola y, si se pidió
 * --log, la fila de D12. Un error de escritura no corta el cierre (dejar
 * la instancia a medio cerrar sería peor); se reporta al final. */
struct journal {
    struct csv_logger lg;
    bool has_log;
    int first_err;
};

static void journal(struct journal *j, const struct csv_record *rec, const char *text) {
    printf("finisher: %s\n", text);
    if (j->has_log) {
        int rc = csv_logger_write(&j->lg, rec);
        if (rc != 0 && j->first_err == 0)
            j->first_err = rc;
    }
}

/* Resumen PROVISIONAL: los totales del corte y la duración del cierre, en
 * el formato largo de D11. El resumen completo (esperas, latencia, p95,
 * throughput, filas por participante) es del Issue #18, que reemplaza esta
 * función por summary_write_csv. Se lee el segmento sin mutex: ya no queda
 * nadie conectado (closing_wait_all_left retornó). */
static int write_provisional_summary(const char *path, const struct instance *inst,
                                     uint64_t close_duration_ns) {
    FILE *f = fopen(path, "w");
    if (f == NULL)
        return -errno;
    uint64_t accepted = inst->enqueue_seq, extracted = inst->dequeue_seq;
    int ok = accepted == extracted + inst->count && inst->count == 0;
    fprintf(f, "metric,role,id,value,unit,note\n");
    fprintf(f, "accepted,instance,0,%" PRIu64 ",msgs,\n", accepted);
    fprintf(f, "extracted,instance,0,%" PRIu64 ",msgs,\n", extracted);
    fprintf(f, "pending,instance,0,%" PRIu32 ",msgs,\n", inst->count);
    fprintf(f, "rejected_puts,instance,0,%" PRIu64 ",count,intentos rechazados por el corte; no son aceptados\n",
            inst->metrics.rejected_puts);
    fprintf(f, "invariant_ok,instance,0,%d,bool,accepted = extracted + pending y pending = 0\n", ok);
    fprintf(f, "close_duration,instance,0,%" PRIu64 ",ns,de CLOSING a liberacion completa\n",
            close_duration_ns);
    fprintf(f, "producers,instance,0,%" PRIu32 ",count,registros de por vida\n", inst->producers.used);
    fprintf(f, "consumers,instance,0,%" PRIu32 ",count,registros de por vida\n", inst->consumers.used);
    int rc = ferror(f) ? -EIO : 0;
    if (fclose(f) != 0 && rc == 0)
        rc = -errno;
    return rc;
}

int main(int argc, char **argv) {
    struct cli_options opt;
    char err[256];
    switch (cli_parse(CLI_FINISHER, argc, argv, &opt, err, sizeof(err))) {
    case CLI_HELP:
        cli_usage(CLI_FINISHER, stdout);
        return EXIT_SUCCESS;
    case CLI_ERROR:
        fprintf(stderr, "finisher: %s\n", err);
        cli_usage(CLI_FINISHER, stderr);
        return EXIT_FAILURE;
    case CLI_OK:
        break;
    }

    struct journal j = { .has_log = opt.log_path != NULL };
    if (j.has_log) {
        int rc = csv_logger_open(&j.lg, opt.log_path, CSV_FINISHER, 0);
        if (rc != 0) {
            fprintf(stderr, "finisher: no se pudo abrir %s: %s\n", opt.log_path, strerror(-rc));
            return EXIT_FAILURE;
        }
    }

    struct csv_record rec;
    char text[192];

    struct instance *inst;
    size_t len;
    enum attach_result ar = instance_attach(opt.name, &inst, &len);
    if (ar != ATTACH_OK) {
        /* Con la puerta cerrada hay otro finisher conduciendo el cierre. */
        if (ar == ATTACH_CLOSING) {
            csv_record_event(&rec, CSV_CLOSE_REJECTED, now_monotonic_ns(), "already_closing");
            journal(&j, &rec, "rechazado: ya hay un cierre en curso");
        }
        if (j.has_log)
            csv_logger_close(&j.lg);
        if (ar == ATTACH_SYS_ERROR)
            fprintf(stderr, "finisher: %s: %s\n", attach_result_str(ar), strerror(errno));
        else
            fprintf(stderr, "finisher: %s: %s\n", opt.name, attach_result_str(ar));
        return EXIT_FAILURE;
    }

    csv_record_event(&rec, CSV_CLOSE_REQUEST, now_monotonic_ns(), NULL);
    snprintf(text, sizeof(text), "solicitud de cierre de %s", opt.name);
    journal(&j, &rec, text);

    struct close_snapshot snap;
    enum close_result cr = closing_begin(inst, &snap);
    if (cr != CLOSE_OK) {
        /* La instancia sigue como estaba: con NO_CONSUMER queda OPEN y se
         * puede reintentar tras levantar un consumidor (D10). */
        csv_record_event(&rec, CSV_CLOSE_REJECTED, now_monotonic_ns(),
                         cr == CLOSE_NO_CONSUMER ? "no_consumer" : "already_closing");
        snprintf(text, sizeof(text), "rechazado: %s", close_result_str(cr));
        journal(&j, &rec, text);
        instance_detach(inst, len);
        if (j.has_log)
            csv_logger_close(&j.lg);
        return EXIT_FAILURE;
    }

    /* Los números del corte se capturaron bajo el mutex (D21). */
    csv_record_event(&rec, CSV_CLOSING, snap.closing_ns, NULL);
    rec.enqueue_seq = snap.accepted;
    rec.dequeue_seq = snap.extracted;
    rec.occupancy = snap.pending;
    snprintf(text, sizeof(text), "CLOSING: aceptados=%" PRIu64 " extraidos=%" PRIu64 " pendientes=%" PRIu32,
             snap.accepted, snap.extracted, snap.pending);
    journal(&j, &rec, text);

    closing_wait_all_left(inst);       /* retorna con el mutex tomado */
    csv_record_event(&rec, CSV_ALL_LEFT, now_monotonic_ns(), NULL);
    journal(&j, &rec, "todos los participantes se retiraron");

    int rel = closing_release(inst, opt.name);
    uint64_t close_ns = now_monotonic_ns() - snap.closing_ns;   /* hasta la liberación (D11) */
    csv_record_event(&rec, CSV_CLOSED, now_monotonic_ns(), rel == 0 ? "released" : "release_error");
    snprintf(text, sizeof(text), "CLOSED: recursos liberados en %.3f ms", (double)close_ns / 1e6);
    journal(&j, &rec, text);

    int sum = write_provisional_summary(opt.summary_path, inst, close_ns);
    memory_unmap(inst, len);

    int close_err = j.has_log ? csv_logger_close(&j.lg) : 0;
    if (j.first_err == 0)
        j.first_err = close_err;

    int status = EXIT_SUCCESS;
    if (rel != 0) {
        fprintf(stderr, "finisher: error liberando recursos: %s\n", strerror(-rel));
        status = EXIT_FAILURE;
    }
    if (sum != 0) {
        fprintf(stderr, "finisher: no se pudo escribir %s: %s\n", opt.summary_path, strerror(-sum));
        status = EXIT_FAILURE;
    }
    if (j.first_err != 0) {
        fprintf(stderr, "finisher: error escribiendo %s: %s\n", opt.log_path, strerror(-j.first_err));
        status = EXIT_FAILURE;
    }
    return status;
}
