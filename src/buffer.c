/* buffer.c — implementacion del buffer circular puro (D2).
 *
 * Sin mutex ni condvars aqui a proposito: estas funciones asumen que quien
 * las llama ya tomo el mutex de la instancia y ya verifico el predicado
 * correspondiente. Los assert() documentan esa precondicion y la verifican
 * en builds de desarrollo; no reemplazan el chequeo real que hace sync.c
 * bajo proteccion (D3).
 */
#include "buffer.h"

#include <assert.h>

bool buffer_is_full(const struct instance *inst) {
    assert(inst->capacity > 0);
    return inst->count == inst->capacity;
}

bool buffer_is_empty(const struct instance *inst) {
    assert(inst->capacity > 0);
    return inst->count == 0;
}

void buffer_push(struct instance *inst, const struct message *msg) {
    assert(inst->capacity > 0);   /* si no, '%' de abajo es division por cero: UB */
    assert(!buffer_is_full(inst));

    inst->messages[inst->tail] = *msg;
    inst->tail = (inst->tail + 1) % inst->capacity;
    inst->count++;
}

void buffer_pop(struct instance *inst, struct message *out) {
    assert(inst->capacity > 0);   /* si no, '%' de abajo es division por cero: UB */
    assert(!buffer_is_empty(inst));

    *out = inst->messages[inst->head];
    inst->head = (inst->head + 1) % inst->capacity;
    inst->count--;
}
