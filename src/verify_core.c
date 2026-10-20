/* verify_core.c — parseo de los CSV de D12 a struct log_row (Issue #7,
 * slice 1). La lógica de cruce/verificación llega en slices siguientes.
 */
#include "verify.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define LOG_FIELD_COUNT 13u

/* Corta buf en 13 campos separándolos por ',', escribiendo '\0' en cada
 * coma (igual que strtok, pero sin saltarse campos vacíos: "a,,c" da
 * {"a", "", "c"}). false si no hay exactamente 13 columnas. */
static bool split_fields(char *buf, char *field[LOG_FIELD_COUNT]) {
    char *p = buf;
    for (unsigned i = 0; i < LOG_FIELD_COUNT; i++) {
        field[i] = p;
        char *comma = strchr(p, ',');
        if (i < LOG_FIELD_COUNT - 1) {
            if (comma == NULL) {
                return false;   /* menos de 13 columnas */
            }
            *comma = '\0';
            p = comma + 1;
        } else if (comma != NULL) {
            return false;   /* coma extra: mas de 13 columnas */
        }
    }
    return true;
}

static bool role_from_str(const char *s, enum log_role *out) {
    if (strcmp(s, "producer") == 0) {
        *out = LOG_ROLE_PRODUCER;
    } else if (strcmp(s, "consumer") == 0) {
        *out = LOG_ROLE_CONSUMER;
    } else if (strcmp(s, "finisher") == 0) {
        *out = LOG_ROLE_FINISHER;
    } else {
        return false;
    }
    return true;
}

static bool event_from_str(const char *s, enum log_event *out) {
    static const struct { const char *name; enum log_event ev; } table[] = {
        {"JOIN", LOG_EVENT_JOIN},
        {"JOIN_REJECTED", LOG_EVENT_JOIN_REJECTED},
        {"PUT", LOG_EVENT_PUT},
        {"PUT_REJECTED", LOG_EVENT_PUT_REJECTED},
        {"GET", LOG_EVENT_GET},
        {"PROCESSED", LOG_EVENT_PROCESSED},
        {"DRAINED", LOG_EVENT_DRAINED},
        {"LEAVE", LOG_EVENT_LEAVE},
        {"CLOSE_REQUEST", LOG_EVENT_CLOSE_REQUEST},
        {"CLOSE_REJECTED", LOG_EVENT_CLOSE_REJECTED},
        {"CLOSING", LOG_EVENT_CLOSING},
        {"ALL_LEFT", LOG_EVENT_ALL_LEFT},
        {"CLOSED", LOG_EVENT_CLOSED},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(s, table[i].name) == 0) {
            *out = table[i].ev;
            return true;
        }
    }
    return false;
}

/* Campo numérico opcional: vacío -> *out = 0. Rechaza signo, espacios y
 * texto no numérico (incluye desbordar el tipo destino). */
static bool parse_u64(const char *tok, uint64_t *out) {
    if (tok[0] == '\0') {
        *out = 0;
        return true;
    }
    if (!isdigit((unsigned char)tok[0])) {
        return false;
    }
    char *end;
    errno = 0;
    unsigned long long v = strtoull(tok, &end, 10);
    if (*end != '\0' || errno == ERANGE) {
        return false;
    }
    *out = v;
    return true;
}

static bool parse_u32(const char *tok, uint32_t *out) {
    uint64_t v;
    if (!parse_u64(tok, &v) || v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

/* Igual que parse_u64/u32 pero el campo es obligatorio: vacío es inválido
 * (role, id, event y t_ns siempre van en toda fila, por D12). */
static bool parse_u32_required(const char *tok, uint32_t *out) {
    return tok[0] != '\0' && parse_u32(tok, out);
}

static bool parse_u64_required(const char *tok, uint64_t *out) {
    return tok[0] != '\0' && parse_u64(tok, out);
}

/* checksum: vacío -> 0; si no, "0x" + hex (strtoul con base 16 acepta el
 * prefijo solo). */
static bool parse_hex_u32(const char *tok, uint32_t *out) {
    if (tok[0] == '\0') {
        *out = 0;
        return true;
    }
    /* Mismo corte que parse_u64: strtoul acepta un signo delante y
     * "-1" devuelve ULONG_MAX sin marcar ERANGE. Hoy el chequeo de abajo
     * (v > UINT32_MAX) lo atrapa porque unsigned long es de 64 bits, pero
     * donde sea de 32 pasaría como 0xffffffff. */
    if (!isxdigit((unsigned char)tok[0])) {
        return false;
    }
    char *end;
    errno = 0;
    unsigned long v = strtoul(tok, &end, 16);
    if (*end != '\0' || errno == ERANGE || v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool parse_payload_ok(const char *tok, int *out) {
    if (tok[0] == '\0') {
        *out = 0;
        return true;
    }
    if ((tok[0] == '0' || tok[0] == '1') && tok[1] == '\0') {
        *out = tok[0] - '0';
        return true;
    }
    return false;
}

bool log_row_parse(const char *line, struct log_row *out) {
    assert(line != NULL && out != NULL);

    size_t len = strlen(line);
    if (len >= LOG_LINE_MAX) {
        return false;   /* linea absurdamente larga: se rechaza, no se trunca en silencio */
    }

    char buf[LOG_LINE_MAX];
    memcpy(buf, line, len + 1);

    char *field[LOG_FIELD_COUNT];
    if (!split_fields(buf, field)) {
        return false;
    }

    struct log_row row = {0};
    if (!role_from_str(field[0], &row.role)) return false;
    if (!parse_u32_required(field[1], &row.id)) return false;
    if (!event_from_str(field[2], &row.event)) return false;
    if (!parse_u64_required(field[3], &row.t_ns)) return false;
    if (!parse_u32(field[4], &row.producer_id)) return false;
    if (!parse_u64(field[5], &row.sequence)) return false;
    if (!parse_u64(field[6], &row.enqueue_seq)) return false;
    if (!parse_u64(field[7], &row.dequeue_seq)) return false;
    if (!parse_u64(field[8], &row.enqueue_time_ns)) return false;
    if (!parse_u32(field[9], &row.occupancy)) return false;
    if (!parse_hex_u32(field[10], &row.checksum)) return false;
    if (!parse_payload_ok(field[11], &row.payload_ok)) return false;

    /* detail es el único campo de texto libre. Si no entra, se rechaza la
     * fila en vez de truncarla: un `detail` cortado a la mitad es dato
     * alterado en silencio, justo lo que esta herramienta debe detectar,
     * no producir. Los valores que define D12 no pasan de 15 caracteres. */
    if (strlen(field[12]) >= sizeof(row.detail)) {
        return false;
    }
    snprintf(row.detail, sizeof(row.detail), "%s", field[12]);

    *out = row;
    return true;
}

/* Lee una línea y le quita el '\n' (y el '\r' de un archivo CRLF). La
 * línea demasiado larga se distingue del error de lectura porque la CLI
 * las reporta distinto; importa atraparla porque fgets devolvería el
 * resto en la llamada siguiente y esa cola entraría al verificador como
 * si fuera otra fila. */
enum read_line_result { RL_OK, RL_EOF, RL_TOO_LONG, RL_IO_ERROR };

static enum read_line_result read_line(FILE *f, char *buf, size_t size) {
    if (!fgets(buf, (int)size, f)) {
        return ferror(f) ? RL_IO_ERROR : RL_EOF;
    }

    size_t len = strlen(buf);
    if (len == size - 1 && buf[len - 1] != '\n') {
        return RL_TOO_LONG;
    }
    if (len > 0 && buf[len - 1] == '\n') {
        buf[--len] = '\0';
    }
    if (len > 0 && buf[len - 1] == '\r') {
        buf[--len] = '\0';
    }
    return RL_OK;
}

const char *log_read_result_str(enum log_read_result r) {
    switch (r) {
    case LOG_READ_OK:        return "ok";
    case LOG_READ_EMPTY:     return "el archivo esta vacio: no tiene ni la cabecera";
    case LOG_READ_BAD_LINE:  return "fila que no cumple el formato de D12, o demasiado larga";
    case LOG_READ_IO_ERROR:  return "error de lectura";
    case LOG_READ_NO_MEMORY: return "sin memoria";
    }
    return "resultado desconocido";
}

enum log_read_result log_rows_read(FILE *f, struct log_row **out_rows,
                                   size_t *out_count, size_t *out_line) {
    assert(f != NULL && out_rows != NULL && out_count != NULL);
    assert(out_line != NULL);

    *out_rows = NULL;
    *out_count = 0;
    *out_line = 0;

    struct log_row *rows = NULL;
    size_t count = 0, cap = 0;
    size_t lineno = 1;              /* la cabecera es la linea 1 */
    enum log_read_result result;

    char line[LOG_LINE_MAX];
    switch (read_line(f, line, sizeof(line))) {
    case RL_OK:
        break;
    case RL_EOF:
        return LOG_READ_EMPTY;      /* ni siquiera la cabecera */
    case RL_TOO_LONG:
        *out_line = lineno;
        return LOG_READ_BAD_LINE;
    case RL_IO_ERROR:
        *out_line = lineno;
        return LOG_READ_IO_ERROR;
    }

    for (;;) {
        lineno++;
        enum read_line_result r = read_line(f, line, sizeof(line));
        if (r == RL_EOF) {
            break;                  /* fin de archivo limpio */
        }
        if (r == RL_TOO_LONG) {
            result = LOG_READ_BAD_LINE;
            goto fail;
        }
        if (r == RL_IO_ERROR) {
            result = LOG_READ_IO_ERROR;
            goto fail;
        }
        if (line[0] == '\0') {
            continue;               /* linea vacia al final del archivo */
        }

        if (count == cap) {
            size_t new_cap = cap ? cap * 2 : 64;
            struct log_row *grown = realloc(rows, new_cap * sizeof(*rows));
            if (!grown) {
                result = LOG_READ_NO_MEMORY;
                goto fail;
            }
            rows = grown;
            cap = new_cap;
        }

        if (!log_row_parse(line, &rows[count])) {
            result = LOG_READ_BAD_LINE;
            goto fail;
        }
        count++;
    }

    *out_rows = rows;
    *out_count = count;
    return LOG_READ_OK;

fail:
    free(rows);
    *out_rows = NULL;
    *out_count = 0;
    /* NO_MEMORY no es culpa de una linea puntual; el resto si. */
    *out_line = (result == LOG_READ_NO_MEMORY) ? 0 : lineno;
    return result;
}

/* ---- Verificación: conservación de identidades (D16, chequeo 1) ------ */

/* Una inserción o una extracción, reducida a lo único que importa para
 * este chequeo: de qué mensaje habla y en qué sentido. */
struct id_event {
    uint32_t producer_id;
    uint64_t sequence;
    bool is_get;
};

static int cmp_id_event(const void *a, const void *b) {
    const struct id_event *x = a;
    const struct id_event *y = b;

    if (x->producer_id != y->producer_id) {
        return x->producer_id < y->producer_id ? -1 : 1;
    }
    if (x->sequence != y->sequence) {
        return x->sequence < y->sequence ? -1 : 1;
    }
    return (int)x->is_get - (int)y->is_get;   /* PUT antes que GET */
}

static bool report_push(struct verify_report *rep, struct violation v) {
    if (rep->count == rep->cap) {
        size_t new_cap = rep->cap ? rep->cap * 2 : 16;
        struct violation *grown = realloc(rep->items,
                                          new_cap * sizeof(*grown));
        if (!grown) {
            return false;
        }
        rep->items = grown;
        rep->cap = new_cap;
    }

    rep->items[rep->count++] = v;
    return true;
}

void verify_report_free(struct verify_report *rep) {
    assert(rep != NULL);
    free(rep->items);
    rep->items = NULL;
    rep->count = 0;
    rep->cap = 0;
}

const char *violation_kind_str(enum violation_kind kind) {
    switch (kind) {
    case VIOLATION_LOST:        return "mensaje perdido: insertado y nunca extraido";
    case VIOLATION_DUPLICATED:  return "mensaje duplicado";
    case VIOLATION_PHANTOM:     return "mensaje fantasma: extraido sin haber sido insertado";
    case VIOLATION_REPEATED_ID: return "identidad repetida: insertado dos veces con el mismo (producer_id, sequence)";
    case VIOLATION_ALTERED:     return "contenido alterado";
    case VIOLATION_PAYLOAD_BAD: return "contenido alterado: el consumidor marco payload_ok = 0";
    case VIOLATION_FIFO_ORDER:  return "orden FIFO roto";
    case VIOLATION_PUT_AFTER_CLOSING:    return "aceptado despues del corte";
    case VIOLATION_CLOSING_INCONSISTENT: return "cierre inconsistente (aceptados != extraidos + pendientes)";
    case VIOLATION_NOT_ALL_DRAINED:      return "cierre incompleto (extracciones != aceptados)";
    }
    return "violacion desconocida";
}

bool verify_identities(const struct log_row *rows, size_t n,
                       struct verify_report *rep) {
    assert(rows != NULL || n == 0);
    assert(rep != NULL);

    size_t total = 0;
    for (size_t i = 0; i < n; i++) {
        if (rows[i].event == LOG_EVENT_PUT || rows[i].event == LOG_EVENT_GET) {
            total++;
        }
    }
    if (total == 0) {
        return true;
    }

    struct id_event *ev = malloc(total * sizeof(*ev));
    if (!ev) {
        return false;
    }

    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (rows[i].event != LOG_EVENT_PUT && rows[i].event != LOG_EVENT_GET) {
            continue;
        }
        ev[k].producer_id = rows[i].producer_id;
        ev[k].sequence = rows[i].sequence;
        ev[k].is_get = (rows[i].event == LOG_EVENT_GET);
        k++;
    }

    /* Ordenar agrupa las dos puntas de cada mensaje sin necesidad de una
     * tabla hash, y de paso fija el orden del reporte. */
    qsort(ev, total, sizeof(*ev), cmp_id_event);

    bool ok = true;
    size_t i = 0;
    while (i < total && ok) {
        uint64_t puts = 0, gets = 0;
        size_t j = i;
        while (j < total && ev[j].producer_id == ev[i].producer_id
                         && ev[j].sequence == ev[i].sequence) {
            if (ev[j].is_get) {
                gets++;
            } else {
                puts++;
            }
            j++;
        }

        /* Una identidad produce a lo sumo una violación, la más cercana a
         * la causa: si el productor emitió dos veces la misma, eso explica
         * cualquier descuadre posterior en las extracciones. */
        enum violation_kind kind;
        bool bad = true;
        if (puts >= 2) {
            kind = VIOLATION_REPEATED_ID;
        } else if (puts == 0) {
            kind = VIOLATION_PHANTOM;
        } else if (gets == 0) {
            kind = VIOLATION_LOST;
        } else if (gets >= 2) {
            kind = VIOLATION_DUPLICATED;
        } else {
            bad = false;
        }

        if (bad) {
            ok = report_push(rep, (struct violation){
                .kind = kind,
                .producer_id = ev[i].producer_id,
                .sequence = ev[i].sequence,
                .gets = gets,
            });
        }

        i = j;
    }

    free(ev);
    return ok;
}

/* ---- Verificación: contenido y orden (D16, chequeos 2 y 3) ----------- */

/* Una inserción reducida a lo que el chequeo de contenido necesita. */
struct put_checksum {
    uint32_t producer_id;
    uint64_t sequence;
    uint32_t checksum;
};

/* Compara solo la identidad: es el criterio del qsort y también la clave
 * del bsearch, por eso ignora el checksum. */
static int cmp_put_checksum(const void *a, const void *b) {
    const struct put_checksum *x = a;
    const struct put_checksum *y = b;

    if (x->producer_id != y->producer_id) {
        return x->producer_id < y->producer_id ? -1 : 1;
    }
    if (x->sequence != y->sequence) {
        return x->sequence < y->sequence ? -1 : 1;
    }
    return 0;
}

bool verify_contents(const struct log_row *rows, size_t n,
                     struct verify_report *rep) {
    assert(rows != NULL || n == 0);
    assert(rep != NULL);

    size_t puts = 0;
    for (size_t i = 0; i < n; i++) {
        if (rows[i].event == LOG_EVENT_PUT) {
            puts++;
        }
    }

    struct put_checksum *index = NULL;
    if (puts > 0) {
        index = malloc(puts * sizeof(*index));
        if (!index) {
            return false;
        }
        size_t k = 0;
        for (size_t i = 0; i < n; i++) {
            if (rows[i].event != LOG_EVENT_PUT) {
                continue;
            }
            index[k].producer_id = rows[i].producer_id;
            index[k].sequence = rows[i].sequence;
            index[k].checksum = rows[i].checksum;
            k++;
        }
        qsort(index, puts, sizeof(*index), cmp_put_checksum);
    }

    bool ok = true;
    for (size_t i = 0; i < n && ok; i++) {
        if (rows[i].event != LOG_EVENT_GET) {
            continue;
        }

        /* Veredicto del propio consumidor, que recalculó fill y checksum
         * sobre el mensaje que recibió. */
        if (rows[i].payload_ok != 1) {
            ok = report_push(rep, (struct violation){
                .kind = VIOLATION_PAYLOAD_BAD,
                .producer_id = rows[i].producer_id,
                .sequence = rows[i].sequence,
            });
            if (!ok) {
                break;
            }
        }

        if (index == NULL) {
            continue;
        }

        struct put_checksum key = {rows[i].producer_id, rows[i].sequence, 0};
        /* Si la identidad se insertó dos veces (REPEATED_ID), bsearch
         * devuelve una de las dos sin decir cuál; comparar contra esa
         * alcanza, porque el chequeo 1 ya reportó el problema de fondo. */
        const struct put_checksum *found = bsearch(&key, index, puts,
                                                   sizeof(*index),
                                                   cmp_put_checksum);
        if (found == NULL) {
            continue;   /* fantasma: ya lo reporta verify_identities */
        }

        if (found->checksum != rows[i].checksum) {
            ok = report_push(rep, (struct violation){
                .kind = VIOLATION_ALTERED,
                .producer_id = rows[i].producer_id,
                .sequence = rows[i].sequence,
                .checksum_put = found->checksum,
                .checksum_get = rows[i].checksum,
            });
        }
    }

    free(index);
    return ok;
}

bool verify_fifo(const struct log_row *rows, size_t n,
                 struct verify_report *rep) {
    assert(rows != NULL || n == 0);
    assert(rep != NULL);

    bool ok = true;
    for (size_t i = 0; i < n && ok; i++) {
        /* Solo los GET: en un PUT, dequeue_seq viene vacío (D12) y
         * compararlo contra enqueue_seq marcaría toda inserción. */
        if (rows[i].event != LOG_EVENT_GET) {
            continue;
        }
        if (rows[i].enqueue_seq == rows[i].dequeue_seq) {
            continue;
        }

        ok = report_push(rep, (struct violation){
            .kind = VIOLATION_FIFO_ORDER,
            .producer_id = rows[i].producer_id,
            .sequence = rows[i].sequence,
            .enqueue_seq = rows[i].enqueue_seq,
            .dequeue_seq = rows[i].dequeue_seq,
        });
    }

    return ok;
}

/* ---- Verificación: corte y cierre (D16, chequeo 4) ------------------- */

bool verify_closing(const struct log_row *rows, size_t n,
                    struct verify_report *rep, bool *out_checked) {
    assert(rows != NULL || n == 0);
    assert(rep != NULL && out_checked != NULL);

    *out_checked = false;

    const struct log_row *closing = NULL;
    for (size_t i = 0; i < n; i++) {
        if (rows[i].event == LOG_EVENT_CLOSING) {
            closing = &rows[i];
            break;
        }
    }
    if (closing == NULL) {
        return true;   /* sin punto de corte no hay nada que contrastar */
    }
    *out_checked = true;

    /* D12: el evento CLOSING lleva la foto del punto de corte. */
    uint64_t accepted = closing->enqueue_seq;
    uint64_t extracted_at_cut = closing->dequeue_seq;
    uint64_t pending = closing->occupancy;

    /* La foto tiene que cerrar sola: lo aceptado está extraído o todavía
     * en el buffer, no hay un tercer lugar donde pueda estar. */
    if (accepted != extracted_at_cut + pending) {
        if (!report_push(rep, (struct violation){
                .kind = VIOLATION_CLOSING_INCONSISTENT,
                .expected = accepted,
                .found = extracted_at_cut + pending,
            })) {
            return false;
        }
    }

    /* Después del corte no se acepta nada más: un PUT con enqueue_seq
     * mayor significa que el productor insertó pese a estar en CLOSING. */
    for (size_t i = 0; i < n; i++) {
        if (rows[i].event != LOG_EVENT_PUT) {
            continue;
        }
        if (rows[i].enqueue_seq <= accepted) {
            continue;
        }
        if (!report_push(rep, (struct violation){
                .kind = VIOLATION_PUT_AFTER_CLOSING,
                .producer_id = rows[i].producer_id,
                .sequence = rows[i].sequence,
                .enqueue_seq = rows[i].enqueue_seq,
                .expected = accepted,
            })) {
            return false;
        }
    }

    /* Al final, los pendientes del corte también salieron. Este conteo es
     * global y no señala un mensaje: cuando el chequeo 1 ya reportó un
     * duplicado o una pérdida, esto lo confirma desde los totales; cuando
     * el chequeo 1 está limpio, delata que faltan filas de log. */
    uint64_t gets = 0;
    for (size_t i = 0; i < n; i++) {
        if (rows[i].event == LOG_EVENT_GET) {
            gets++;
        }
    }
    if (gets != accepted) {
        return report_push(rep, (struct violation){
            .kind = VIOLATION_NOT_ALL_DRAINED,
            .expected = accepted,
            .found = gets,
        });
    }

    return true;
}
