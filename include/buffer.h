/* buffer.h — buffer circular puro, sin sincronizacion.
 *
 * Decision documentada en docs/decisiones_diseno.md (D2). Estas funciones
 * NO son thread-safe por si mismas: asumen que quien las llama ya tomo
 * el mutex de la instancia y ya verifico el predicado correspondiente
 * (buffer_is_full / buffer_is_empty) antes de llamar a push/pop. Tampoco
 * asignan enqueue_seq, dequeue_seq ni enqueue_time_ns — esos campos deben
 * venir ya completos en el *msg que se le pasa a buffer_push; asignarlos
 * es responsabilidad de la capa de sincronizacion (ver D3), porque
 * requiere estar bajo proteccion para que el orden global no se pierda.
 */
#ifndef SOA_P2_BUFFER_H
#define SOA_P2_BUFFER_H

#include <stdbool.h>

#include "memory.h"

/* true si count == capacity. Precondicion: ninguna (solo lee). */
bool buffer_is_full(const struct instance *inst);

/* true si count == 0. Precondicion: ninguna (solo lee). */
bool buffer_is_empty(const struct instance *inst);

/* Copia *msg a la casilla tail, avanza tail (modulo capacity) e
 * incrementa count. Precondicion: !buffer_is_full(inst). */
void buffer_push(struct instance *inst, const struct message *msg);

/* Copia la casilla head a *out, avanza head (modulo capacity) y
 * decrementa count. Precondicion: !buffer_is_empty(inst). */
void buffer_pop(struct instance *inst, struct message *out);

#endif /* SOA_P2_BUFFER_H */
