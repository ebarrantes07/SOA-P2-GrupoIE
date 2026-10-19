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
    VIOLATION_REPEATED_ID   /* insertado dos veces con la misma identidad */
};

struct violation {
    enum violation_kind kind;
    uint32_t producer_id;   /* identidad del mensaje afectado */
    uint64_t sequence;
    uint64_t gets;          /* extracciones halladas para esa identidad */
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

#endif /* SOA_P2_VERIFY_H */
