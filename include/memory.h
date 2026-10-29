/* memory.h — layout del segmento de memoria compartida y módulo que lo
 * crea, mapea y elimina.
 *
 * Decisiones documentadas en docs/decisiones_diseno.md (D1 layout, D13
 * módulo de memoria). No cambiar este archivo sin agregar una entrada nueva
 * ahí explicando por qué.
 */
#ifndef SOA_P2_MEMORY_H
#define SOA_P2_MEMORY_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metrics.h"
#include "participants.h"

#define INSTANCE_MAGIC      0xDEADBEEFu   /* arbitrario, != 0, facil de ver en un hexdump/gdb */
#define INSTANCE_VERSION    2u   /* 2: cuarta condvar `closing` (D18, #11) */
#define PAYLOAD_FILL_SIZE   16             /* ver D1, seccion "Payload: metadata + checksum" */

#define INSTANCE_MIN_CAPACITY  1u
#define INSTANCE_MAX_CAPACITY  1024u

/* Nombre de instancia: '/' + 1..30 caracteres de [A-Za-z0-9_-]. 31 es el
 * límite de macOS (PSHMNAMLEN) y cabe holgado en el de Linux; ver D13. */
#define INSTANCE_NAME_MAX      31u

/* Los atómicos del segmento solo sincronizan entre procesos si son
 * lock-free (address-free); uno con lock interno usaría un lock privado de
 * cada proceso. Revisión de #29, ver D13. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "los atomicos de 32 bits deben ser lock-free entre procesos");
_Static_assert(sizeof(uint32_t) == sizeof(int),
               "ATOMIC_INT_LOCK_FREE debe cubrir a uint32_t");

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
    uint32_t         reserved;             /* relleno explicito, siempre 0; ver D13 */
    uint64_t         sequence;             /* por productor */
    uint64_t         enqueue_seq;          /* global, asignado bajo proteccion, empieza en 1 */
    uint64_t         enqueue_time_ns;      /* CLOCK_MONOTONIC, capturado bajo proteccion */
    struct payload   payload;
};

/* ---- Estado protegido y propiedad de los recursos (D29) ----------------
 *
 * Cómo se protege cada campo de struct instance:
 *
 *   Campo                         Protección
 *   ----------------------------  ---------------------------------------------
 *   magic                         atómico; el creator lo escribe una sola vez,
 *                                 al final (release), y se lee con acquire (D1)
 *   version, capacity             inmutables después de publicar magic: se leen
 *                                 sin mutex una vez validado magic (D1, D13)
 *   mutex, not_full, not_empty,   las primitivas mismas: el creator las inicia
 *   closing, all_left             antes de publicar y el finisher las destruye
 *                                 cuando refs == 1 (D9, D10)
 *   state                         mutex (lo escriben el creator antes de
 *                                 publicar y el finisher en el corte y al cerrar)
 *   head, tail, count, messages[] mutex (buffer.h asume el mutex tomado, D2)
 *   enqueue_seq, dequeue_seq      mutex (asignados en sync_put / sync_get, D3)
 *   active_producers/consumers    mutex (registro y retiro, D8)
 *   producers, consumers (tablas) mutex (registro y retiro, D8)
 *   metrics                       mutex mientras hay participantes; el finisher
 *                                 la lee sin mutex cuando ya no queda nadie
 *                                 conectado (D11, D26)
 *   refs                          atómico: se incrementa con CAS sin mutex al
 *                                 conectarse; se decrementa y se cierra la
 *                                 puerta bajo el mutex (D9)
 *
 * Propiedad de los recursos (quién crea y quién destruye):
 *
 *   Recurso                       Crea                     Destruye / libera
 *   ----------------------------  -----------------------  --------------------------
 *   nombre del segmento           creator (shm_open        finisher (shm_unlink en
 *                                 O_CREAT|O_EXCL)          closing_release)
 *   objeto de memoria             creator (ftruncate)      el kernel, al último munmap
 *   mapeo de cada proceso         cada proceso (mmap)      el mismo proceso (munmap)
 *   primitivas de sincronización  creator (sync_init_      finisher (closing_release,
 *                                 primitives, antes de     con refs == 1)
 *                                 publicar magic)
 *   referencia de conexión        instance_attach          instance_detach (el
 *                                                          finisher no la suelta:
 *                                                          libera todo)
 *   casilla de registro           participant_register     nunca se libera: queda
 *                                                          SLOT_LEFT de por vida (D8)
 *   CSV de cada proceso           ese proceso              ese proceso
 *
 * Las precondiciones de cada operación están en su header (buffer.h,
 * sync.h, participants.h, memory.h).
 */
struct instance {
    _Atomic uint32_t magic;        /* se escribe ultimo, ver D1, "Publicacion segura" */
    uint32_t         version;
    uint32_t         capacity;     /* 1..1024, fija toda la vida de la instancia */
    int              state;        /* enum instance_state */

    pthread_mutex_t  mutex;        /* PTHREAD_PROCESS_SHARED */
    pthread_cond_t   not_full;     /* disponibilidad de espacio */
    pthread_cond_t   not_empty;    /* disponibilidad de mensajes */
    pthread_cond_t   closing;      /* productores en pausa (#13); solo despierta al cortar */
    pthread_cond_t   all_left;     /* el finisher espera refs == 1 (solo el), ver D9 */

    uint32_t  head, tail, count;
    uint64_t  enqueue_seq, dequeue_seq;

    uint32_t  active_producers, active_consumers;

    /* Conexiones vivas + bit de puerta cerrada (REFS_GATE_CLOSED). Se toma
     * con CAS en instance_attach sin el mutex; ver D9. */
    _Atomic uint32_t refs;

    /* Registro de por vida por rol (unicidad de ids), ver D8. */
    struct participant_table producers;
    struct participant_table consumers;

    /* Agregados para el resumen del finisher, ver D11. */
    struct instance_metrics metrics;

    struct message messages[];     /* capacity * sizeof(struct message), al final */
};

_Static_assert(sizeof(struct payload) == 32, "payload debe medir 32 bytes (D1)");
_Static_assert(sizeof(struct message) == 64, "message debe medir 64 bytes (D13)");

/* ---- Módulo de memoria (src/memory.c) --------------------------------- */

enum memory_result {
    MEM_OK = 0,
    MEM_INVALID_NAME,       /* no cumple el formato de INSTANCE_NAME_MAX */
    MEM_INVALID_CAPACITY,   /* fuera de 1..1024 */
    MEM_EXISTS,             /* ya hay una instancia con ese nombre; no se tocó */
    MEM_NOT_FOUND,          /* no existe un segmento con ese nombre */
    MEM_NOT_READY,          /* magic != INSTANCE_MAGIC: creator sin terminar o segmento ajeno */
    MEM_INCOMPATIBLE,       /* versión, capacidad o tamaño no coinciden con este binario */
    MEM_SYS_ERROR           /* otra llamada al sistema falló; errno queda con la causa */
};

/* Bytes del segmento para una capacidad: cabecera + capacity mensajes. */
size_t instance_segment_size(uint32_t capacity);

/* true si name cumple el formato de nombre de instancia. */
bool instance_name_is_valid(const char *name);

/* Mensaje legible para un resultado, para los errores de los ejecutables. */
const char *memory_result_str(enum memory_result r);

/* Crea e inicializa la instancia siguiendo los 8 pasos de D1 y la publica
 * escribiendo magic al final. Si falla, deshace solo lo que creó esta
 * llamada (con MEM_EXISTS no toca nada). En MEM_OK deja la instancia
 * mapeada en *out con su tamaño en *map_len. */
enum memory_result memory_create(const char *name, uint32_t capacity,
                                 struct instance **out, size_t *map_len);

/* Abre y mapea una instancia existente y valida magic (acquire), versión,
 * capacidad y tamaño del segmento antes de devolverla. No toma referencia
 * de conexión ni toca primitivas: eso es instance_attach (D9). En
 * cualquier resultado distinto de MEM_OK no queda nada mapeado. */
enum memory_result memory_open(const char *name, struct instance **out,
                               size_t *map_len);

/* Desmapea una instancia. Devuelve 0 o -errno. */
int memory_unmap(struct instance *inst, size_t map_len);

/* Elimina el nombre del segmento (shm_unlink). El objeto vive hasta el
 * último munmap. Devuelve 0 o -errno. */
int memory_unlink(const char *name);

#endif /* SOA_P2_MEMORY_H */
