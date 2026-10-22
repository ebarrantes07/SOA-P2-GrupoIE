/* participants.c — conexión a la instancia (D9) y registro de participantes
 * (D8). Las operaciones de cierre de participants.h (closing_*) las
 * implementan los Issues #16 y #17.
 */
#include "participants.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "clock.h"
#include "memory.h"
#include "metrics.h"
#include "sync.h"

const char *attach_result_str(enum attach_result r) {
    switch (r) {
    case ATTACH_OK:           return "ok";
    case ATTACH_INVALID_NAME: return "nombre invalido";
    case ATTACH_NOT_FOUND:    return "no existe una instancia con ese nombre";
    case ATTACH_NOT_READY:    return "la instancia no esta lista";
    case ATTACH_INCOMPATIBLE: return "la instancia no es compatible con este binario";
    case ATTACH_CLOSING:      return "la instancia ya esta cerrandose; no admite conexiones";
    case ATTACH_SYS_ERROR:    return "error del sistema";
    }
    return "resultado desconocido";
}

const char *register_result_str(enum register_result r) {
    switch (r) {
    case REG_OK:           return "ok";
    case REG_DUPLICATE_ID: return "id duplicado: ya se uso en este rol durante la vida de la instancia";
    case REG_NOT_OPEN:     return "la instancia ya no admite participantes (no esta en OPEN)";
    case REG_TABLE_FULL:   return "se alcanzo el maximo de registros para este rol";
    }
    return "resultado desconocido";
}

/* ---- Conexión (D9) ---------------------------------------------------- */

static enum attach_result from_memory_result(enum memory_result r) {
    switch (r) {
    case MEM_OK:               return ATTACH_OK;
    case MEM_INVALID_NAME:     return ATTACH_INVALID_NAME;
    case MEM_NOT_FOUND:        return ATTACH_NOT_FOUND;
    case MEM_NOT_READY:        return ATTACH_NOT_READY;
    case MEM_INCOMPATIBLE:     return ATTACH_INCOMPATIBLE;
    case MEM_INVALID_CAPACITY:
    case MEM_EXISTS:
    case MEM_SYS_ERROR:        break;
    }
    return ATTACH_SYS_ERROR;
}

enum attach_result instance_attach(const char *name, struct instance **out,
                                   size_t *map_len) {
    struct instance *inst;
    size_t len;

    /* Mapear y validar magic (acquire), versión, capacidad y tamaño (D13).
     * Hasta acá no se tocó ninguna primitiva. */
    enum memory_result mr = memory_open(name, &inst, &len);
    if (mr != MEM_OK)
        return from_memory_result(mr);

    /* Tomar la referencia con CAS, sin el mutex (D9). Si la puerta está
     * cerrada no se toca ninguna primitiva: el finisher podría haberlas
     * destruido ya, y este mapeo propio sigue siendo válido hasta munmap. */
    uint32_t v = atomic_load(&inst->refs);
    do {
        if (v & REFS_GATE_CLOSED) {
            memory_unmap(inst, len);
            return ATTACH_CLOSING;
        }
    } while (!atomic_compare_exchange_weak(&inst->refs, &v, v + 1));

    *out = inst;
    *map_len = len;
    return ATTACH_OK;
}

void instance_detach(struct instance *inst, size_t map_len) {
    /* Bajo el mutex y con signal: el finisher duerme en all_left con el
     * predicado sobre refs, así que no puede perderse este cambio (D9). */
    sync_lock(inst);
    atomic_fetch_sub(&inst->refs, 1);
    sync_signal_all_left(inst);
    sync_unlock(inst);
    memory_unmap(inst, map_len);
}

/* ---- Registro (D8) ---------------------------------------------------- */

static struct participant_table *table_of(struct instance *inst, enum role role) {
    return role == ROLE_PRODUCER ? &inst->producers : &inst->consumers;
}

static uint32_t *active_of(struct instance *inst, enum role role) {
    return role == ROLE_PRODUCER ? &inst->active_producers : &inst->active_consumers;
}

enum register_result participant_register(struct instance *inst, enum role role,
                                          uint32_t id, int *slot) {
    struct participant_table *t = table_of(inst, role);
    enum register_result r = REG_OK;

    sync_lock(inst);

    /* Orden de D8: estado, duplicado, espacio. Ningún rechazo escribe en
     * la tabla ni en los contadores. */
    if (inst->state != STATE_OPEN) {
        r = REG_NOT_OPEN;
    } else {
        /* Recorre también las casillas SLOT_LEFT: el id queda reservado
         * de por vida aunque su dueño ya se haya retirado. */
        for (uint32_t i = 0; i < t->used; i++) {
            if (t->slots[i].id == id) {
                r = REG_DUPLICATE_ID;
                break;
            }
        }
        if (r == REG_OK && t->used == MAX_PARTICIPANTS_PER_ROLE)
            r = REG_TABLE_FULL;
    }

    if (r == REG_OK) {
        uint64_t now = now_monotonic_ns();
        uint32_t i = t->used++;
        struct participant_slot *s = &t->slots[i];
        s->id = id;
        s->state = SLOT_ACTIVE;
        s->pid = (int32_t)getpid();
        s->joined_ns = now;
        (*active_of(inst, role))++;
        if (inst->metrics.first_start_ns == 0)
            inst->metrics.first_start_ns = now;   /* inicio del throughput (D11) */
        *slot = (int)i;
    }

    sync_unlock(inst);
    return r;
}

void participant_leave(struct instance *inst, enum role role, int slot,
                       const struct participant_stats *stats) {
    struct participant_table *t = table_of(inst, role);

    sync_lock(inst);
    /* Retirarse dos veces, o con una casilla ajena, descontaría de más el
     * contador de activos y el finisher esperaría para siempre o cerraría
     * antes de tiempo. Es un error de quien programa: se aborta (D18). */
    if (slot < 0 || (uint32_t)slot >= t->used || t->slots[slot].state != SLOT_ACTIVE) {
        fprintf(stderr, "participants: participant_leave con casilla invalida (%d)\n", slot);
        abort();
    }
    struct participant_slot *s = &t->slots[slot];
    s->state = SLOT_LEFT;
    s->left_ns = now_monotonic_ns();
    if (stats != NULL) {
        s->ops = stats->ops;
        s->wait_ns = stats->wait_ns;
        s->wait_count = stats->wait_count;
        s->lock_ns = stats->lock_ns;
    }
    (*active_of(inst, role))--;
    sync_unlock(inst);
}
