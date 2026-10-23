/* participants.h — conexión a la instancia, registro de participantes y
 * máquina de estados OPEN/CLOSING/CLOSED.
 *
 * Decisiones documentadas en docs/decisiones_diseno.md (D8 registro, D9
 * conexiones en curso, D10 máquina de estados y cierre). No cambiar este
 * archivo sin agregar una entrada nueva ahí explicando por qué.
 *
 * Este header solo declara tipos que viven en el segmento y las firmas de
 * las operaciones; no incluye memory.h para que memory.h pueda incluirlo a
 * él sin dependencia circular.
 */
#ifndef SOA_P2_PARTICIPANTS_H
#define SOA_P2_PARTICIPANTS_H

#include <stddef.h>
#include <stdint.h>

struct instance;            /* definida en memory.h */
struct participant_stats;   /* definida en metrics.h */

/* ---- Tipos compartidos (viven dentro de struct instance) -------------- */

/* Registros de por vida por rol. Cubre el mínimo del enunciado (32 por rol)
 * con holgura para el profesor; ver D8 sobre por qué es de por vida. */
#define MAX_PARTICIPANTS_PER_ROLE  64u

/* Bit alto de struct instance.refs: puerta de conexión cerrada (ver D9). */
#define REFS_GATE_CLOSED   0x80000000u
#define REFS_COUNT_MASK    0x7FFFFFFFu

enum role { ROLE_PRODUCER = 0, ROLE_CONSUMER = 1 };

enum slot_state {
    SLOT_FREE   = 0,    /* casilla nunca usada (el segmento nace en ceros) */
    SLOT_ACTIVE = 1,    /* registrado y todavía no se retiró */
    SLOT_LEFT   = 2     /* se retiró; el id sigue reservado de por vida */
};

/* Una casilla por registro exitoso. Las métricas se copian al retirarse,
 * así el finisher arma el resumen por proceso sin leer los CSV. */
struct participant_slot {
    uint32_t id;
    uint32_t state;              /* enum slot_state */
    int32_t  pid;                /* solo informativo, para depurar */
    uint32_t reserved;           /* relleno explícito, siempre 0 */
    uint64_t joined_ns;          /* CLOCK_MONOTONIC al registrarse */
    uint64_t left_ns;            /* CLOCK_MONOTONIC al retirarse */
    uint64_t ops;                /* mensajes aceptados (prod.) o extraídos (cons.) */
    uint64_t wait_ns;            /* espera bloqueado por espacio (prod.) o datos (cons.) */
    uint64_t wait_count;         /* cuántas veces durmió en esa condvar */
    uint64_t lock_ns;            /* tiempo total adquiriendo el mutex */
};

struct participant_table {
    uint32_t used;               /* casillas ocupadas, nunca decrece */
    uint32_t reserved;
    struct participant_slot slots[MAX_PARTICIPANTS_PER_ROLE];
};

/* ---- Conexión (attach/detach) ----------------------------------------- */

enum attach_result {
    ATTACH_OK = 0,
    ATTACH_INVALID_NAME,    /* el nombre no cumple el formato de D13 */
    ATTACH_NOT_FOUND,       /* shm_open falló con ENOENT: no existe la instancia */
    ATTACH_NOT_READY,       /* magic != INSTANCE_MAGIC: creator no terminó o no es nuestra */
    ATTACH_INCOMPATIBLE,    /* versión, capacidad o tamaño no coinciden (D13) */
    ATTACH_CLOSING,         /* puerta cerrada: la instancia ya está en CLOSING */
    ATTACH_SYS_ERROR        /* otro error de sistema; errno queda con la causa */
};

/* Abre y mapea la instancia `name`, valida magic/version y toma una
 * referencia en inst->refs. Solo después de ATTACH_OK el llamador puede
 * tocar el mutex o las condvars. En cualquier otro resultado no queda nada
 * mapeado ni referencia tomada. */
enum attach_result instance_attach(const char *name, struct instance **out,
                                   size_t *map_len);

/* Mensajes legibles para los ejecutables. */
const char *attach_result_str(enum attach_result r);

/* Suelta la referencia (bajo el mutex, con signal a all_left) y desmapea.
 * Todo proceso que obtuvo ATTACH_OK la llama exactamente una vez, salvo el
 * finisher que conduce el cierre, que libera con closing_release() y
 * munmap(). */
void instance_detach(struct instance *inst, size_t map_len);

/* ---- Registro --------------------------------------------------------- */

enum register_result {
    REG_OK = 0,
    REG_DUPLICATE_ID,       /* el id ya se usó en este rol durante la vida de la instancia */
    REG_NOT_OPEN,           /* la instancia ya no está en OPEN */
    REG_TABLE_FULL          /* se agotaron las MAX_PARTICIPANTS_PER_ROLE casillas */
};

const char *register_result_str(enum register_result r);

/* Bajo el mutex: verifica state == OPEN, unicidad del id y espacio en la
 * tabla; ocupa la casilla, incrementa active_producers/active_consumers y,
 * si es el primer trabajador, fija metrics.first_start_ns. Devuelve el
 * índice de la casilla en *slot. Un rechazo no modifica la tabla. */
enum register_result participant_register(struct instance *inst, enum role role,
                                          uint32_t id, int *slot);

/* Bajo el mutex: marca la casilla SLOT_LEFT, copia las métricas locales,
 * decrementa el contador de activos del rol. No suelta la referencia de
 * conexión: después se llama instance_detach(). */
void participant_leave(struct instance *inst, enum role role, int slot,
                       const struct participant_stats *stats);

/* ---- Cierre (finisher) ------------------------------------------------ */

enum close_result {
    CLOSE_OK = 0,           /* esta llamada hizo OPEN -> CLOSING: es el finalizador */
    CLOSE_NO_CONSUMER,      /* precondición falló: la instancia sigue OPEN */
    CLOSE_ALREADY_CLOSING   /* otro finisher ya condujo el corte */
};

const char *close_result_str(enum close_result r);

/* Lo que vale en el punto de corte. Existe porque D12 pide esos tres
 * numeros "en el punto de corte" para la fila CLOSING del CSV, y despues
 * de soltar el mutex ya no se pueden obtener: los consumidores siguen
 * drenando (D10), asi que `extracted` crece y `pending` baja. Leerlos
 * afuera sin el mutex ademas los deja mutuamente inconsistentes, porque
 * las dos lecturas no son atomicas entre si.
 *
 * Mismo patron que struct op_snapshot en sync.h, y por la misma razon:
 * capturar bajo proteccion y escribir despues. */
struct close_snapshot {
    uint64_t accepted;     /* enqueue_seq: aceptados hasta el corte */
    uint64_t extracted;    /* dequeue_seq: extraidos hasta el corte */
    uint32_t pending;      /* count: en el buffer al cortar */
    uint64_t closing_ns;   /* la marca guardada en metrics.closing_start_ns */
};

/* Bajo el mutex, en una sola región crítica: evalúa la precondición
 * (active_consumers >= 1, o active_producers == 0 && count == 0); si se
 * cumple pasa a CLOSING, cierra la puerta de conexión, guarda
 * metrics.closing_start_ns y hace sync_broadcast_all(). Este cambio de
 * estado es el punto de corte para put y para el registro.
 *
 * `snap` puede ser NULL si al llamador no le interesan los numeros del
 * corte (los necesita el ejecutable para la fila CLOSING, no #17). Si no
 * es NULL se pone en cero al entrar, asi que queda definido en cualquier
 * camino; un rechazo lo deja en cero, porque D12 no pide numeros en la
 * fila CLOSE_REJECTED. */
enum close_result closing_begin(struct instance *inst,
                                struct close_snapshot *snap);

/* Toma el mutex y duerme en all_left mientras (refs & REFS_COUNT_MASK) > 1,
 * es decir, mientras quede alguien conectado además del propio finisher.
 * Retorna con el mutex tomado; desde ahí nadie más puede volver a tocar
 * las primitivas. */
void closing_wait_all_left(struct instance *inst);

/* Pasa a CLOSED, suelta el mutex, destruye las primitivas y hace
 * shm_unlink(name). NO desmapea: el finisher todavía lee los agregados
 * para el resumen y luego hace munmap (ver D10, orden del cierre).
 * Precondición: closing_wait_all_left() ya retornó con el mutex tomado.
 * Devuelve 0 o -errno; tras un error sigue liberando lo que pueda y
 * reporta el primero. */
int closing_release(struct instance *inst, const char *name);

#endif /* SOA_P2_PARTICIPANTS_H */
