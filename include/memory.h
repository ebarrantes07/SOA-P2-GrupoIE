/* memory.h — layout del segmento de memoria compartida.
 *
 * Decisión documentada en docs/decisiones_diseno.md (D1). No cambiar este
 * archivo sin agregar una entrada nueva ahí explicando por qué.
 */
#ifndef SOA_P2_MEMORY_H
#define SOA_P2_MEMORY_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#define INSTANCE_MAGIC      0xDEADBEEFu   /* arbitrario, != 0, facil de ver en un hexdump/gdb */
#define INSTANCE_VERSION    1u
#define PAYLOAD_FILL_SIZE   16             /* ver D1, seccion "Payload: metadata + checksum" */

enum instance_state { STATE_OPEN = 0, STATE_CLOSING = 1, STATE_CLOSED = 2 };

/* Campos ordenados de mayor a menor alineacion (8, 4, 1 bytes) a proposito:
 * asi sizeof(struct payload) da 32 sin relleno invisible del compilador. */
struct payload {
    uint64_t sequence;                    /* copia: ver D1, "redundancia deliberada" */
    uint32_t producer_id;                 /* copia: ver D1, "redundancia deliberada" */
    uint32_t checksum;                    /* FNV-1a de 32 bits sobre los campos de abajo */
    uint8_t  fill[PAYLOAD_FILL_SIZE];     /* fill[i] = (producer_id + sequence + i) % 256 */
};                                         /* sizeof == 32 bytes, sin relleno del compilador */

struct message {
    uint32_t         producer_id;
    uint64_t         sequence;             /* por productor */
    uint64_t         enqueue_seq;          /* global, asignado bajo proteccion, empieza en 1 */
    uint64_t         enqueue_time_ns;      /* CLOCK_MONOTONIC, capturado bajo proteccion */
    struct payload   payload;
};

struct instance {
    _Atomic uint32_t magic;        /* se escribe ultimo, ver D1, "Publicacion segura" */
    uint32_t         version;
    uint32_t         capacity;     /* 1..1024, fija toda la vida de la instancia */
    int              state;        /* enum instance_state */

    pthread_mutex_t  mutex;        /* PTHREAD_PROCESS_SHARED */
    pthread_cond_t   not_full;     /* disponibilidad de espacio */
    pthread_cond_t   not_empty;    /* disponibilidad de mensajes */
    pthread_cond_t   all_left;     /* para que el finisher espere activos == 0 */

    uint32_t  head, tail, count;
    uint64_t  enqueue_seq, dequeue_seq;

    uint32_t  active_producers, active_consumers;

    /* tabla de ids usados por rol (unicidad de por vida): se agrega en #12 */

    struct message messages[];     /* capacity * sizeof(struct message), al final */
};

#endif /* SOA_P2_MEMORY_H */
