/* payload.c — unica implementacion del payload verificable de D1 (D22). */
#include "payload.h"

#include <string.h>

void payload_fill(uint8_t fill[PAYLOAD_FILL_SIZE], uint32_t producer_id,
                  uint64_t sequence) {
    for (unsigned i = 0; i < PAYLOAD_FILL_SIZE; i++) {
        /* El % 256 de D1 es el byte bajo de la suma, y el truncamiento a
         * uint8_t ES ese modulo. Hacer la suma en 32 bits daria exactamente
         * lo mismo: el desborde de un unsigned envuelve y nunca altera el
         * byte bajo (medido con una mutacion: no cambia ningun valor del
         * vector congelado). El uint64_t es por claridad de lectura, no por
         * correccion. */
        uint64_t v = (uint64_t)producer_id + sequence + i;
        fill[i] = (uint8_t)(v % 256u);
    }
}

static uint32_t fnv1a(uint32_t hash, const uint8_t *bytes, size_t len) {
    for (size_t i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= PAYLOAD_FNV_PRIME;   /* uint32_t: el desborde envuelve, es el algoritmo */
    }
    return hash;
}

uint32_t payload_checksum(uint32_t producer_id, uint64_t sequence,
                          const uint8_t fill[PAYLOAD_FILL_SIZE]) {
    /* Serializacion explicita de los campos de identidad. memcpy y no un
     * cast a uint8_t*: evita cualquier duda de aliasing estricto y el
     * compilador lo convierte en dos movimientos. */
    uint8_t ident[sizeof(uint64_t) + sizeof(uint32_t)];
    memcpy(ident, &sequence, sizeof sequence);
    memcpy(ident + sizeof sequence, &producer_id, sizeof producer_id);

    uint32_t hash = fnv1a(PAYLOAD_FNV_OFFSET_BASIS, ident, sizeof ident);
    return fnv1a(hash, fill, PAYLOAD_FILL_SIZE);
}

void payload_build(struct payload *p, uint32_t producer_id, uint64_t sequence) {
    p->producer_id = producer_id;
    p->sequence = sequence;
    payload_fill(p->fill, producer_id, sequence);
    /* Sobre p->fill recien generado: el checksum describe lo que de verdad
     * se va a insertar. */
    p->checksum = payload_checksum(producer_id, sequence, p->fill);
}

bool payload_verify(const struct message *msg) {
    const struct payload *p = &msg->payload;

    /* 1. Chequeo cruzado: el sobre contra las copias del payload. Va
     * primero porque si la identidad no coincide no hay un "deberia" con
     * el cual comparar el resto. */
    if (p->producer_id != msg->producer_id || p->sequence != msg->sequence)
        return false;

    /* 2. Relleno esperado para esa identidad, byte a byte. */
    uint8_t expected[PAYLOAD_FILL_SIZE];
    payload_fill(expected, msg->producer_id, msg->sequence);
    if (memcmp(expected, p->fill, PAYLOAD_FILL_SIZE) != 0)
        return false;

    /* 3. Checksum sobre el fill que VIAJO en el mensaje (que en este punto
     * ya se sabe igual al esperado): detecta que se haya corrompido el
     * propio campo checksum.
     *
     * Pasar `expected` aqui seria equivalente, pero SOLO mientras el paso 2
     * siga delante. Quitar el paso 2 y hashear `expected` a la vez deja
     * pasar cualquier fill corrupto; la prueba every_fill_byte_is_detected
     * cubre esa combinacion. */
    return p->checksum == payload_checksum(msg->producer_id, msg->sequence, p->fill);
}
