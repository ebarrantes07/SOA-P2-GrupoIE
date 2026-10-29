/* logger.c — CSV de eventos por proceso (D12, D17).
 *
 * La tabla EVENTS traduce la tabla "Eventos y campos que llevan" de D12:
 * qué roles pueden emitir cada evento y qué columnas lleva. Es la única
 * fuente de esa regla; cambiar D12 obliga a cambiar esta tabla.
 */
#include "logger.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "memory.h"
#include "sync.h"

/* Columnas opcionales de D12 (role, id, event y t_ns van siempre). */
enum {
    F_PRODUCER_ID = 1u << 0,
    F_SEQUENCE    = 1u << 1,
    F_ENQ_SEQ     = 1u << 2,
    F_DEQ_SEQ     = 1u << 3,
    F_ENQ_TIME    = 1u << 4,
    F_OCCUPANCY   = 1u << 5,
    F_CHECKSUM    = 1u << 6,
    F_PAYLOAD_OK  = 1u << 7,
    F_DETAIL      = 1u << 8,
};

#define R_PROD    (1u << CSV_PRODUCER)
#define R_CONS    (1u << CSV_CONSUMER)
#define R_FIN     (1u << CSV_FINISHER)
#define R_WORKERS (R_PROD | R_CONS)

static const struct {
    const char *name;
    unsigned    roles;
    unsigned    fields;
} EVENTS[CSV_EVENT_COUNT] = {
    [CSV_JOIN]           = { "JOIN",           R_WORKERS, F_DETAIL },
    [CSV_JOIN_REJECTED]  = { "JOIN_REJECTED",  R_WORKERS, F_DETAIL },
    [CSV_PUT]            = { "PUT",            R_PROD,
                             F_PRODUCER_ID | F_SEQUENCE | F_ENQ_SEQ | F_ENQ_TIME |
                             F_OCCUPANCY | F_CHECKSUM },
    [CSV_PUT_REJECTED]   = { "PUT_REJECTED",   R_PROD, F_PRODUCER_ID | F_SEQUENCE | F_DETAIL },
    [CSV_GET]            = { "GET",            R_CONS,
                             F_PRODUCER_ID | F_SEQUENCE | F_ENQ_SEQ | F_DEQ_SEQ |
                             F_ENQ_TIME | F_OCCUPANCY | F_CHECKSUM | F_PAYLOAD_OK },
    [CSV_PROCESSED]      = { "PROCESSED",      R_CONS, F_PRODUCER_ID | F_SEQUENCE | F_DEQ_SEQ },
    [CSV_DRAINED]        = { "DRAINED",        R_CONS, F_OCCUPANCY },
    [CSV_LEAVE]          = { "LEAVE",          R_WORKERS, F_DETAIL },
    [CSV_CLOSE_REQUEST]  = { "CLOSE_REQUEST",  R_FIN, 0 },
    [CSV_CLOSE_REJECTED] = { "CLOSE_REJECTED", R_FIN, F_DETAIL },
    [CSV_CLOSING]        = { "CLOSING",        R_FIN, F_ENQ_SEQ | F_DEQ_SEQ | F_OCCUPANCY },
    [CSV_ALL_LEFT]       = { "ALL_LEFT",       R_FIN, 0 },
    [CSV_CLOSED]         = { "CLOSED",         R_FIN, F_DETAIL },
};

static const char *const ROLE_NAMES[] = {
    [CSV_PRODUCER] = "producer", [CSV_CONSUMER] = "consumer", [CSV_FINISHER] = "finisher",
};

/* ---- Armado de la fila ------------------------------------------------ */

/* Copia src a dst (de CSV_DETAIL_MAX bytes) truncando y reemplazando por
 * '_' las comas, saltos de línea y caracteres de control. */
static void sanitize_detail(char *dst, const char *src) {
    size_t i = 0;
    if (src != NULL) {
        for (; src[i] != '\0' && i < CSV_DETAIL_MAX - 1; i++) {
            unsigned char c = (unsigned char)src[i];
            dst[i] = (c == ',' || c < 0x20 || c == 0x7f) ? '_' : (char)c;
        }
    }
    dst[i] = '\0';
}

/* Agrega texto a buf llevando la posición; marca desborde en *ok. */
static void put(char *buf, size_t len, size_t *pos, bool *ok, const char *fmt, ...) {
    if (!*ok)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *pos, len - *pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= len - *pos)
        *ok = false;
    else
        *pos += (size_t)n;
}

int csv_format_row(char *buf, size_t len, enum csv_role role, uint32_t id,
                   const struct csv_record *rec) {
    if ((unsigned)rec->event >= CSV_EVENT_COUNT || (unsigned)role > CSV_FINISHER ||
        !(EVENTS[rec->event].roles & (1u << role)) || len == 0)
        return -1;

    unsigned f = EVENTS[rec->event].fields;
    size_t pos = 0;
    bool ok = true;

    put(buf, len, &pos, &ok, "%s,%" PRIu32 ",%s,%" PRIu64 ",",
        ROLE_NAMES[role], id, EVENTS[rec->event].name, rec->t_ns);

    /* Cada columna opcional: el valor si D12 la define, vacía si no. */
    if (f & F_PRODUCER_ID) put(buf, len, &pos, &ok, "%" PRIu32, rec->producer_id);
    put(buf, len, &pos, &ok, ",");
    if (f & F_SEQUENCE)    put(buf, len, &pos, &ok, "%" PRIu64, rec->sequence);
    put(buf, len, &pos, &ok, ",");
    if (f & F_ENQ_SEQ)     put(buf, len, &pos, &ok, "%" PRIu64, rec->enqueue_seq);
    put(buf, len, &pos, &ok, ",");
    if (f & F_DEQ_SEQ)     put(buf, len, &pos, &ok, "%" PRIu64, rec->dequeue_seq);
    put(buf, len, &pos, &ok, ",");
    if (f & F_ENQ_TIME)    put(buf, len, &pos, &ok, "%" PRIu64, rec->enqueue_time_ns);
    put(buf, len, &pos, &ok, ",");
    if (f & F_OCCUPANCY)   put(buf, len, &pos, &ok, "%" PRIu32, rec->occupancy);
    put(buf, len, &pos, &ok, ",");
    if (f & F_CHECKSUM)    put(buf, len, &pos, &ok, "0x%08" PRIx32, rec->checksum);
    put(buf, len, &pos, &ok, ",");
    if (f & F_PAYLOAD_OK)  put(buf, len, &pos, &ok, "%d", rec->payload_ok ? 1 : 0);
    put(buf, len, &pos, &ok, ",");
    if (f & F_DETAIL) {
        /* detail se sanea al armarse (csv_record_set_detail), pero un
         * registro llenado a mano podría no haberlo pasado por ahí. */
        char clean[CSV_DETAIL_MAX];
        sanitize_detail(clean, rec->detail);
        put(buf, len, &pos, &ok, "%s", clean);
    }

    return ok ? (int)pos : -1;
}

/* ---- Archivo ---------------------------------------------------------- */

int csv_logger_open(struct csv_logger *lg, const char *path,
                    enum csv_role role, uint32_t id) {
    lg->f = NULL;
    /* No se usa fopen(path, "w"): trunca al abrir, antes de saber si otro
     * proceso vivo escribe en ese archivo. Un participante rechazado (id
     * duplicado, instancia inexistente) con el mismo --log que uno en
     * curso le vaciaba el CSV a ese otro, que seguía escribiendo en su
     * posición y dejaba un hueco de bytes nulos (#19). Se abre sin
     * truncar, se toma un candado de escritura de todo el archivo sin
     * esperar, y solo con el candado se trunca. El candado se suelta solo
     * al cerrar o al morir el proceso, así que no queda pegado. */
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd == -1)
        return -errno;
    struct flock lk = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
    if (fcntl(fd, F_SETLK, &lk) == -1) {
        int e = (errno == EACCES || errno == EAGAIN) ? EBUSY : errno;
        close(fd);
        return -e;
    }
    if (ftruncate(fd, 0) == -1) {
        int e = errno;
        close(fd);
        return -e;
    }
    FILE *f = fdopen(fd, "w");
    if (f == NULL) {
        int e = errno;
        close(fd);
        return -e;
    }
    if (fprintf(f, "%s\n", CSV_HEADER) < 0) {
        int e = errno ? errno : EIO;
        fclose(f);
        return -e;
    }
    lg->f = f;
    lg->role = role;
    lg->id = id;
    return 0;
}

const char *csv_logger_open_str(int rc) {
    if (rc == -EBUSY)
        return "otro proceso en ejecucion ya escribe en ese archivo";
    return strerror(-rc);
}

int csv_logger_write(struct csv_logger *lg, const struct csv_record *rec) {
    /* Una fila de D12 ronda los 150 caracteres; CSV_LINE_MAX sobra. */
    char line[CSV_LINE_MAX];
    int n = csv_format_row(line, sizeof(line) - 1, lg->role, lg->id, rec);
    if (n < 0)
        return -EINVAL;
    line[n] = '\n';
    /* Una sola escritura por fila: la línea queda entera en el buffer. */
    if (fwrite(line, 1, (size_t)n + 1, lg->f) != (size_t)n + 1)
        return errno ? -errno : -EIO;
    return 0;
}

int csv_logger_close(struct csv_logger *lg) {
    if (lg->f == NULL)
        return 0;
    int rc = 0;
    if (fflush(lg->f) != 0)
        rc = errno ? -errno : -EIO;
    if (fclose(lg->f) != 0 && rc == 0)
        rc = errno ? -errno : -EIO;
    lg->f = NULL;
    return rc;
}

/* ---- Armado de registros ---------------------------------------------- */

void csv_record_set_detail(struct csv_record *rec, const char *detail) {
    sanitize_detail(rec->detail, detail);
}

void csv_record_event(struct csv_record *rec, enum csv_event event,
                      uint64_t t_ns, const char *detail) {
    memset(rec, 0, sizeof(*rec));
    rec->event = event;
    rec->t_ns = t_ns;
    csv_record_set_detail(rec, detail);
}

void csv_record_put(struct csv_record *rec, const struct message *msg,
                    const struct op_snapshot *snap) {
    memset(rec, 0, sizeof(*rec));
    rec->event = CSV_PUT;
    rec->t_ns = snap->time_ns;
    rec->producer_id = msg->producer_id;
    rec->sequence = msg->sequence;
    rec->enqueue_seq = snap->seq;
    rec->enqueue_time_ns = msg->enqueue_time_ns;
    rec->occupancy = snap->occupancy;
    rec->checksum = msg->payload.checksum;
}

void csv_record_get(struct csv_record *rec, const struct message *msg,
                    const struct op_snapshot *snap, bool payload_ok) {
    memset(rec, 0, sizeof(*rec));
    rec->event = CSV_GET;
    rec->t_ns = snap->time_ns;
    rec->producer_id = msg->producer_id;
    rec->sequence = msg->sequence;
    rec->enqueue_seq = msg->enqueue_seq;
    rec->dequeue_seq = snap->seq;
    rec->enqueue_time_ns = msg->enqueue_time_ns;
    rec->occupancy = snap->occupancy;
    rec->checksum = msg->payload.checksum;
    rec->payload_ok = payload_ok;
}
