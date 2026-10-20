/* verify.h — parseo y verificación de los CSV de productor/consumidor/
 * finisher (Issue #7), post-mortem sobre archivos ya cerrados.
 *
 * Formato exacto de columnas y eventos documentado en
 * docs/decisiones_diseno.md (D12). No cambiar este archivo sin revisar esa
 * entrada primero.
 *
 * Esta primera parte (slice 1) solo parsea filas a struct log_row; la
 * lógica de verificación (cruce PUT/GET, FIFO, cierre) llega en slices
 * siguientes y vive en funciones aparte sobre este mismo arreglo.
 */
#ifndef SOA_P2_VERIFY_H
#define SOA_P2_VERIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum log_role {
    LOG_ROLE_PRODUCER,
    LOG_ROLE_CONSUMER,
    LOG_ROLE_FINISHER
};

enum log_event {
    LOG_EVENT_JOIN,
    LOG_EVENT_JOIN_REJECTED,
    LOG_EVENT_PUT,
    LOG_EVENT_PUT_REJECTED,
    LOG_EVENT_GET,
    LOG_EVENT_PROCESSED,
    LOG_EVENT_DRAINED,
    LOG_EVENT_LEAVE,
    LOG_EVENT_CLOSE_REQUEST,
    LOG_EVENT_CLOSE_REJECTED,
    LOG_EVENT_CLOSING,
    LOG_EVENT_ALL_LEFT,
    LOG_EVENT_CLOSED
};

#define LOG_DETAIL_MAX 48u

/* Largo máximo de una línea del CSV, contando el '\0'. Una fila de D12
 * ronda los 100 caracteres; lo que no entre acá se rechaza (ver
 * log_row_parse y log_rows_read) en vez de partirse en dos filas. */
#define LOG_LINE_MAX 512u

/* Una fila ya parseada del CSV (columnas en D12). Los campos que D12 marca
 * como "no aplica a este evento -> vacío" quedan en 0 (numéricos) o "" (
 * detail): decidir qué campo importa para cada `event` es trabajo de la
 * lógica de verificación, no de este parseo, así que no hay valores
 * centinela distintos de 0/"". */
struct log_row {
    enum log_role role;
    uint32_t id;
    enum log_event event;
    uint64_t t_ns;

    uint32_t producer_id;
    uint64_t sequence;
    uint64_t enqueue_seq;
    uint64_t dequeue_seq;
    uint64_t enqueue_time_ns;
    uint32_t occupancy;
    uint32_t checksum;        /* ya decodificado del hex "0x........" */
    int payload_ok;           /* 0 o 1; 0 también si el campo venía vacío */
    char detail[LOG_DETAIL_MAX];
};

/* Parsea una línea (sin el salto de línea final) con el formato de D12.
 * Devuelve false si no tiene exactamente 13 columnas o algún campo no es
 * válido para su tipo (p. ej. `role` desconocido, numérico con texto,
 * payload_ok distinto de 0/1). No valida reglas de negocio entre campos
 * (p. ej. que producer_id > 0): eso es de la capa de verificación. */
bool log_row_parse(const char *line, struct log_row *out);

/* Lee todas las filas de datos de un CSV con cabecera (D12) ya abierto en
 * `f`. Descarta la primera línea sin validar su contenido (se asume que ya
 * es la cabecera documentada). Con éxito, deja en *out_rows un arreglo
 * malloc'eado de *out_count filas (NULL si *out_count == 0: archivo vacío
 * de datos, no es error) y devuelve true; el llamador hace free(*out_rows).
 * Con error (archivo vacío del todo, fila malformada, o memoria), deja
 * *out_rows en NULL, *out_count en 0 y devuelve false. */
bool log_rows_read(FILE *f, struct log_row **out_rows, size_t *out_count);

/* ---- Verificación (D15) ----------------------------------------------- */

enum violation_kind {
    VIOLATION_LOST,         /* insertado y nunca extraído */
    VIOLATION_DUPLICATED,   /* extraído más de una vez */
    VIOLATION_PHANTOM,      /* extraído sin haber sido insertado */
    VIOLATION_REPEATED_ID,  /* insertado dos veces con la misma identidad */
    VIOLATION_ALTERED,      /* el checksum al extraer no es el de al insertar */
    VIOLATION_PAYLOAD_BAD,  /* el propio consumidor marcó payload_ok = 0 */
    VIOLATION_FIFO_ORDER,   /* extraído en una posición distinta a la de inserción */
    VIOLATION_PUT_AFTER_CLOSING,    /* aceptado después del punto de corte */
    VIOLATION_CLOSING_INCONSISTENT, /* la foto del finisher no cierra */
    VIOLATION_NOT_ALL_DRAINED       /* al final, aceptados != extraídos */
};

/* Los campos de contexto valen 0 para los tipos que no los usan; cada uno
 * lo llena un solo chequeo. */
struct violation {
    enum violation_kind kind;
    uint32_t producer_id;   /* identidad del mensaje afectado */
    uint64_t sequence;
    uint64_t gets;          /* chequeo 1: extracciones halladas */
    uint32_t checksum_put;  /* chequeo 2: ALTERED */
    uint32_t checksum_get;
    uint64_t enqueue_seq;   /* chequeo 3: FIFO_ORDER; chequeo 4: el PUT tardío */
    uint64_t dequeue_seq;
    uint64_t expected;      /* chequeo 4: lo que decía el evento CLOSING */
    uint64_t found;         /* chequeo 4: lo que dicen los logs */
};

/* Acumula las violaciones de todos los chequeos: cada uno agrega las
 * suyas al mismo reporte, así una corrida mala se diagnostica de una
 * sola pasada en vez de arreglar una cosa, volver a correr y encontrar
 * la siguiente. Se inicializa en cero ({0}) y se libera con
 * verify_report_free. */
struct verify_report {
    struct violation *items;
    size_t count;
    size_t cap;             /* interno: capacidad reservada en items */
};

void verify_report_free(struct verify_report *rep);

/* Texto corto y estable para el reporte, en el estilo de
 * memory_result_str. */
const char *violation_kind_str(enum violation_kind kind);

/* Cruza los PUT con los GET de `rows` por (producer_id, sequence) y
 * agrega a *rep toda identidad que no se conserve: perdida, extraída de
 * más, extraída sin haber entrado, o insertada dos veces. Los demás
 * eventos se ignoran; en particular PUT_REJECTED, que lleva identidad
 * pero corresponde a un mensaje que nunca entró al buffer.
 *
 * Las violaciones quedan ordenadas por (producer_id, sequence), y cada
 * identidad produce a lo sumo una. Devuelve false solo si se quedó sin
 * memoria: encontrar violaciones no es un error de esta función, es su
 * resultado. */
bool verify_identities(const struct log_row *rows, size_t n,
                       struct verify_report *rep);

/* Chequeo 2 de D15: contenido. Por cada `GET`, exige que el consumidor no
 * haya marcado `payload_ok = 0` y que el checksum coincida con el que
 * registró el `PUT` de esa identidad. Un `GET` sin `PUT` se saltea: ya lo
 * reporta verify_identities como fantasma, no tiene sentido contarlo dos
 * veces.
 *
 * A diferencia del chequeo 1, un mismo `GET` puede producir dos
 * violaciones: que el consumidor detectara contenido inconsistente y que
 * además el checksum no sea el del productor son hechos distintos y
 * conviene ver los dos. Las violaciones salen en el orden en que
 * aparecen los `GET` en `rows`. */
bool verify_contents(const struct log_row *rows, size_t n,
                     struct verify_report *rep);

/* Chequeo 3 de D15: orden FIFO. Como hay una sola cola, el mensaje
 * extraído k-ésimo tiene que ser el insertado k-ésimo: por cada `GET`,
 * `enqueue_seq == dequeue_seq`. Solo mira filas `GET`; en un `PUT` el
 * campo `dequeue_seq` viene vacío por definición (D12). */
bool verify_fifo(const struct log_row *rows, size_t n,
                 struct verify_report *rep);

/* Chequeo 4 de D15: corte y cierre, a partir del evento `CLOSING` del
 * finisher, que según D12 lleva los aceptados en `enqueue_seq`, los
 * extraídos en `dequeue_seq` y los pendientes en `occupancy`. Verifica
 * que esa foto cierre (aceptados == extraídos + pendientes), que ningún
 * `PUT` tenga un `enqueue_seq` mayor al corte, y que al final el total de
 * extracciones iguale a los aceptados.
 *
 * Sin evento `CLOSING` no hay con qué contrastar: no reporta nada y deja
 * *out_checked en false, para que el llamador pueda avisar que el cierre
 * quedó sin verificar en vez de darlo por bueno. Si hubiera más de un
 * `CLOSING` usa el primero; detectar esa repetición es del protocolo de
 * cierre, no de este chequeo. */
bool verify_closing(const struct log_row *rows, size_t n,
                    struct verify_report *rep, bool *out_checked);

#endif /* SOA_P2_VERIFY_H */
