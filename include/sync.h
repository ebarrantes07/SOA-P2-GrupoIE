/* sync.h — capa de sincronizacion: exclusion mutua y esperas bloqueantes.
 *
 * Decision documentada en docs/decisiones_diseno.md (D3). Es la unica capa
 * que toca el mutex y las condvars; buffer.h asume que ya estan tomados.
 *
 * El protocolo de cierre (precondicion del finisher, espera sobre all_left,
 * confirmacion de retirada) se disena en los Issues #16/#17 y agrega aqui
 * sus propias firmas; esta version solo cubre put/get y el ciclo de vida de
 * las primitivas.
 */
#ifndef SOA_P2_SYNC_H
#define SOA_P2_SYNC_H

#include <stdint.h>

#include "memory.h"

/* ---- Ciclo de vida de las primitivas ---------------------------------- */

/* Inicializa mutex y las cuatro condvars con PTHREAD_PROCESS_SHARED y
 * reloj CLOCK_MONOTONIC. La llama solo el creator, antes de publicar el
 * magic. Devuelve 0, o -errno dejando deshecho lo que alcanzo a crear.
 *
 * Init y destroy devuelven el error en vez de abortar, a diferencia de
 * las operaciones de abajo: el creator y el finisher todavia pueden
 * informar y limpiar (ver D18). */
int sync_init_primitives(struct instance *inst);

/* Destruye mutex y condvars. La llama solo el finisher, y solo cuando ya
 * garantizo que nadie puede volver a usarlas. Devuelve 0 o -errno. */
int sync_destroy_primitives(struct instance *inst);

/* ---- Exclusion mutua para otros modulos -------------------------------- */

/* Para que el registro de participantes (#12) y el finisher (#16/#17)
 * construyan sus operaciones protegidas sobre el mismo mutex. Se usan
 * siempre en pares; no anidar.
 *
 * Devuelven void: un fallo del mutex significa protocolo roto y aborta
 * con el nombre de la llamada y la causa (D18). */
void sync_lock(struct instance *inst);
void sync_unlock(struct instance *inst);

/* Despierta a todos los bloqueados en las cuatro condvars. Precondicion:
 * mutex tomado. La usa el finisher al pasar a CLOSING, porque ese cambio
 * vuelve falso el predicado de TODOS los que esperan (ver D3). */
void sync_broadcast_all(struct instance *inst);

/* ---- Operaciones bloqueantes sobre el buffer --------------------------- */

/* Datos capturados DENTRO de la region critica para que el llamador los
 * escriba al CSV despues de soltar el mutex (exigencia del enunciado:
 * "capturar bajo proteccion los datos necesarios para registrar un evento
 * y escribirlos despues"). Puede pasarse NULL si no se va a registrar. */
struct op_snapshot {
    uint64_t seq;          /* enqueue_seq en put, dequeue_seq en get */
    uint64_t time_ns;      /* CLOCK_MONOTONIC, tomado bajo proteccion */
    uint32_t occupancy;    /* count justo despues de la operacion */
    /* Agregados por el Diseño B (D11) para el resumen por proceso: */
    uint64_t wait_ns;      /* tiempo dormido en not_full/not_empty en esta op */
    uint32_t wait_count;   /* cuantas veces durmio en esta op */
    uint64_t lock_ns;      /* tiempo de adquisicion del mutex en esta op */
};

enum put_result {
    PUT_OK = 0,              /* mensaje aceptado: cuenta para las metricas */
    PUT_REJECTED_CLOSING     /* rechazado por el corte: NO cuenta como aceptado */
};

enum get_result {
    GET_OK = 0,              /* se extrajo un mensaje */
    GET_DRAINED              /* buffer vacio y no pueden llegar mas: salir */
};

/* Inserta *msg, bloqueando mientras el buffer este lleno y la instancia
 * siga OPEN. Asigna msg->enqueue_seq y msg->enqueue_time_ns bajo
 * proteccion. */
enum put_result sync_put(struct instance *inst, struct message *msg,
                         struct op_snapshot *snap);

/* Extrae el mensaje mas viejo en *out, bloqueando mientras el buffer este
 * vacio y la instancia siga OPEN. Sigue drenando en CLOSING mientras
 * queden mensajes. */
enum get_result sync_get(struct instance *inst, struct message *out,
                         struct op_snapshot *snap);

#endif /* SOA_P2_SYNC_H */
