/* verify.c — CLI del verificador de los CSV (Issue #7, D15).
 *
 * Toma N archivos de cualquier rol, los cruza como un solo conjunto de
 * eventos y corre los cuatro chequeos. Acá vive toda la E/S y el formato
 * del reporte; la lógica está en verify_core.c, que no imprime nada para
 * poder probarse con CSV en memoria.
 */
#include "verify.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Una corrida de estrés con un bug real puede dar miles de violaciones;
 * listarlas todas no ayuda a nadie. Se muestran las primeras y se dice
 * cuántas quedaron. */
#define MAX_PRINTED 20

static void usage(FILE *out) {
    fprintf(out,
        "uso: verify <log1.csv> [log2.csv ...]\n"
        "\n"
        "Cruza los CSV de productores, consumidores y finisher (formato D12)\n"
        "y verifica que el buffer haya conservado los mensajes: sin perdidas\n"
        "ni duplicados, sin contenido alterado, en orden FIFO y con un cierre\n"
        "consistente. El orden de los archivos no importa.\n"
        "\n"
        "Codigos de salida:\n"
        "  0  sin violaciones\n"
        "  1  se encontro al menos una violacion\n"
        "  2  no se pudo leer una entrada o el uso es incorrecto\n");
}

static void print_violation(const struct violation *v) {
    const char *what = violation_kind_str(v->kind);

    /* Sin `default:` a proposito: agregar un tipo de violacion y olvidar
     * imprimirlo rompe la compilacion con -Werror=switch. */
    switch (v->kind) {
    case VIOLATION_DUPLICATED:
        printf("  (productor %u, secuencia %llu) %s: extraido %llu veces\n",
               v->producer_id, (unsigned long long)v->sequence, what,
               (unsigned long long)v->gets);
        break;
    case VIOLATION_ALTERED:
        printf("  (productor %u, secuencia %llu) %s: 0x%08x al insertar, "
               "0x%08x al extraer\n",
               v->producer_id, (unsigned long long)v->sequence, what,
               v->checksum_put, v->checksum_get);
        break;
    case VIOLATION_FIFO_ORDER:
        printf("  (productor %u, secuencia %llu) %s: insertado en la "
               "posicion %llu, extraido en la %llu\n",
               v->producer_id, (unsigned long long)v->sequence, what,
               (unsigned long long)v->enqueue_seq,
               (unsigned long long)v->dequeue_seq);
        break;
    case VIOLATION_PUT_AFTER_CLOSING:
        printf("  (productor %u, secuencia %llu) %s: enqueue_seq %llu, "
               "el corte fue en %llu\n",
               v->producer_id, (unsigned long long)v->sequence, what,
               (unsigned long long)v->enqueue_seq,
               (unsigned long long)v->expected);
        break;
    case VIOLATION_CLOSING_INCONSISTENT:
    case VIOLATION_NOT_ALL_DRAINED:
        printf("  %s: CLOSING dice %llu, los logs dicen %llu\n", what,
               (unsigned long long)v->expected,
               (unsigned long long)v->found);
        break;
    case VIOLATION_LOST:
    case VIOLATION_PHANTOM:
    case VIOLATION_REPEATED_ID:
    case VIOLATION_PAYLOAD_BAD:
        printf("  (productor %u, secuencia %llu) %s\n",
               v->producer_id, (unsigned long long)v->sequence, what);
        break;
    }
}

/* Copia `n` filas al final del arreglo acumulado, creciéndolo al doble
 * tantas veces como haga falta. */
static bool append_rows(struct log_row **all, size_t *count, size_t *cap,
                        const struct log_row *src, size_t n) {
    if (*count + n > *cap) {
        size_t new_cap = *cap ? *cap : 64;
        while (new_cap < *count + n) {
            new_cap *= 2;
        }
        struct log_row *grown = realloc(*all, new_cap * sizeof(**all));
        if (!grown) {
            return false;
        }
        *all = grown;
        *cap = new_cap;
    }

    memcpy(*all + *count, src, n * sizeof(*src));
    *count += n;
    return true;
}

/* Lee un archivo y lo agrega al acumulado. Devuelve false ya habiendo
 * informado el motivo por stderr. */
static bool read_into(const char *path, struct log_row **all, size_t *count,
                      size_t *cap) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "verify: %s: %s\n", path, strerror(errno));
        return false;
    }

    struct log_row *rows = NULL;
    size_t n = 0, line = 0;
    enum log_read_result r = log_rows_read(f, &rows, &n, &line);
    fclose(f);

    if (r != LOG_READ_OK) {
        if (line > 0) {
            fprintf(stderr, "verify: %s, linea %zu: %s\n", path, line,
                    log_read_result_str(r));
        } else {
            fprintf(stderr, "verify: %s: %s\n", path,
                    log_read_result_str(r));
        }
        return false;
    }

    bool ok = append_rows(all, count, cap, rows, n);
    if (!ok) {
        fprintf(stderr, "verify: sin memoria al juntar %s\n", path);
    }
    free(rows);
    return ok;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        }
    }
    if (argc < 2) {
        fprintf(stderr, "verify: falta al menos un archivo CSV\n");
        usage(stderr);
        return 2;
    }

    struct log_row *all = NULL;
    size_t count = 0, cap = 0;

    for (int i = 1; i < argc; i++) {
        if (!read_into(argv[i], &all, &count, &cap)) {
            free(all);
            return 2;
        }
    }

    struct verify_report rep = {0};
    bool closing_checked = false;
    /* El corto circuito es intencional: si un chequeo se queda sin
     * memoria, el reporte ya no es completo y seguir no aporta. */
    bool ok = verify_identities(all, count, &rep)
              && verify_contents(all, count, &rep)
              && verify_fifo(all, count, &rep)
              && verify_closing(all, count, &rep, &closing_checked);

    int status;
    if (!ok) {
        fprintf(stderr, "verify: sin memoria durante la verificacion\n");
        status = 2;
    } else {
        printf("verify: %d archivo(s), %zu filas de log, cierre %s\n",
               argc - 1, count,
               closing_checked ? "verificado"
                               : "SIN VERIFICAR (falta el evento CLOSING)");

        size_t shown = rep.count < MAX_PRINTED ? rep.count : MAX_PRINTED;
        for (size_t i = 0; i < shown; i++) {
            print_violation(&rep.items[i]);
        }
        if (rep.count > shown) {
            printf("  ... y %zu violacion(es) mas\n", rep.count - shown);
        }

        if (rep.count == 0) {
            printf("verify: sin violaciones\n");
            status = 0;
        } else {
            printf("verify: %zu violacion(es)\n", rep.count);
            status = 1;
        }
    }

    verify_report_free(&rep);
    free(all);
    return status;
}
