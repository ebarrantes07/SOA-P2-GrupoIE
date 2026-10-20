/* logger.h — CSV de eventos por proceso (formato de D12).
 *
 * Decisión documentada en docs/decisiones_diseno.md (D16). El patrón de uso
 * separa captura y escritura, como pide el enunciado:
 *
 *   sync_lock / sync_put(..., &snap) / sync_unlock     <- región crítica
 *   csv_record_put(&rec, &msg, &snap);                 <- copia barata de lo capturado
 *   csv_logger_write(&lg, &rec);                       <- E/S, ya sin el mutex
 *
 * Quien llama llena un struct csv_record con lo que tiene; el logger escribe
 * solo las columnas que D12 define para ese evento y deja vacías las demás.
 * Los nombres llevan prefijo csv_ para no chocar con los del verificador
 * (verify.h), que se enlaza en los mismos binarios.
 */
#ifndef SOA_P2_LOGGER_H
#define SOA_P2_LOGGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct message;             /* memory.h */
struct op_snapshot;         /* sync.h */

enum csv_role { CSV_PRODUCER = 0, CSV_CONSUMER, CSV_FINISHER };

enum csv_event {
    CSV_JOIN = 0,
    CSV_JOIN_REJECTED,
    CSV_PUT,
    CSV_PUT_REJECTED,
    CSV_GET,
    CSV_PROCESSED,
    CSV_DRAINED,
    CSV_LEAVE,
    CSV_CLOSE_REQUEST,
    CSV_CLOSE_REJECTED,
    CSV_CLOSING,
    CSV_ALL_LEFT,
    CSV_CLOSED,
    CSV_EVENT_COUNT
};

/* detail cabe en 47 caracteres + '\0': el mismo límite que lee el
 * verificador (LOG_DETAIL_MAX en verify.h). Lo que sobra se trunca. */
#define CSV_DETAIL_MAX 48u

/* La cabecera de D12, sin salto de línea. */
#define CSV_HEADER "role,id,event,t_ns,producer_id,sequence,enqueue_seq,dequeue_seq," \
                   "enqueue_time_ns,occupancy,checksum,payload_ok,detail"

/* Un evento ya capturado. Los campos que D12 no define para `event` se
 * ignoran al escribir, así que no hace falta limpiarlos. */
struct csv_record {
    enum csv_event event;
    uint64_t t_ns;              /* CLOCK_MONOTONIC; en PUT/GET, el tomado bajo el mutex */
    uint32_t producer_id;
    uint64_t sequence;
    uint64_t enqueue_seq;
    uint64_t dequeue_seq;
    uint64_t enqueue_time_ns;
    uint32_t occupancy;
    uint32_t checksum;
    bool     payload_ok;
    char     detail[CSV_DETAIL_MAX];
};

/* Un CSV abierto. role e id se escriben en cada fila. */
struct csv_logger {
    FILE         *f;
    enum csv_role role;
    uint32_t      id;           /* 0 para el finisher */
};

/* Crea (o trunca) el archivo y escribe la cabecera. Devuelve 0 o -errno;
 * si falla no queda nada abierto. */
int csv_logger_open(struct csv_logger *lg, const char *path,
                    enum csv_role role, uint32_t id);

/* Escribe una fila. Se llama SIN el mutex de la instancia: es E/S.
 * Devuelve 0, -EINVAL si el evento no corresponde al rol del logger
 * (D12), o -errno si falla la escritura. */
int csv_logger_write(struct csv_logger *lg, const struct csv_record *rec);

/* Vacía el buffer y cierra. Devuelve 0 o el primer -errno (un error de
 * escritura diferida aparece recién aquí). Siempre deja lg->f en NULL. */
int csv_logger_close(struct csv_logger *lg);

/* Arma la fila completa en buf (sin salto de línea). La usa
 * csv_logger_write y las pruebas. Devuelve la longitud, o -1 si no cabe o
 * el evento no corresponde al rol. */
int csv_format_row(char *buf, size_t len, enum csv_role role, uint32_t id,
                   const struct csv_record *rec);

/* ---- Armado de registros ---------------------------------------------- */

/* Copia detail truncando a CSV_DETAIL_MAX - 1 y reemplazando por '_' las
 * comas, saltos de línea y caracteres de control, que romperían el CSV. */
void csv_record_set_detail(struct csv_record *rec, const char *detail);

/* Registro de un evento sin datos de mensaje (JOIN, LEAVE, CLOSING, ...). */
void csv_record_event(struct csv_record *rec, enum csv_event event,
                      uint64_t t_ns, const char *detail);

/* PUT aceptado: identidad, enqueue_seq y enqueue_time_ns del mensaje ya
 * insertado, ocupación del snapshot tomado bajo el mutex. */
void csv_record_put(struct csv_record *rec, const struct message *msg,
                    const struct op_snapshot *snap);

/* GET: mensaje extraído, dequeue_seq / marca / ocupación del snapshot y
 * el resultado de recalcular el payload (D12). */
void csv_record_get(struct csv_record *rec, const struct message *msg,
                    const struct op_snapshot *snap, bool payload_ok);

#endif /* SOA_P2_LOGGER_H */
