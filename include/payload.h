/* payload.h — payload verificable: relleno determinista y checksum FNV-1a.
 *
 * Decisiones documentadas en docs/decisiones_diseno.md (D1 contenido y
 * eleccion del algoritmo, D22 este modulo y el alcance de payload_ok).
 *
 * Existe como modulo aparte porque lo usan los dos extremos: el productor
 * (#14) llena el payload antes de insertar y el consumidor (#15) lo
 * recalcula al extraer para escribir la columna payload_ok de D12. Dos
 * implementaciones del mismo FNV-1a tendrian que coincidir byte a byte, o
 * todos los payload_ok saldrian en 0 en una corrida sana.
 */
#ifndef SOA_P2_PAYLOAD_H
#define SOA_P2_PAYLOAD_H

#include <stdbool.h>
#include <stdint.h>

#include "memory.h"   /* struct payload, struct message, PAYLOAD_FILL_SIZE */

/* Constantes de FNV-1a de 32 bits (D1). Expuestas para que una prueba
 * pueda recalcular el hash sin copiar los numeros a mano. */
#define PAYLOAD_FNV_OFFSET_BASIS  0x811c9dc5u
#define PAYLOAD_FNV_PRIME         0x01000193u

/* Bytes que entran al hash: sequence (8) + producer_id (4) + fill (16). */
#define PAYLOAD_CHECKSUM_INPUT_LEN \
    (sizeof(uint64_t) + sizeof(uint32_t) + (size_t)PAYLOAD_FILL_SIZE)

_Static_assert(PAYLOAD_CHECKSUM_INPUT_LEN == 28,
               "D1 fija 28 bytes de entrada al checksum: 8 + 4 + 16");

/* Relleno determinista de D1: fill[i] = (producer_id + sequence + i) % 256.
 * Depende solo de la identidad, asi que cualquiera puede recalcularlo. */
void payload_fill(uint8_t fill[PAYLOAD_FILL_SIZE], uint32_t producer_id,
                  uint64_t sequence);

/* FNV-1a sobre los 28 bytes de entrada, serializados campo por campo y no
 * como un hash directo sobre struct payload: asi el valor no depende de
 * como el compilador decida alinear o rellenar la estructura (D1).
 *
 * Toma `fill` por separado en vez de recalcularlo: el consumidor necesita
 * hashear el relleno que REALMENTE viajo en el mensaje, no el que deberia
 * haber viajado, o un relleno corrupto daria el mismo checksum. */
uint32_t payload_checksum(uint32_t producer_id, uint64_t sequence,
                          const uint8_t fill[PAYLOAD_FILL_SIZE]);

/* Llena los cuatro campos del payload para una identidad: copia
 * producer_id y sequence (la redundancia deliberada de D1), genera fill y
 * calcula checksum. La usa el productor antes de sync_put. */
void payload_build(struct payload *p, uint32_t producer_id, uint64_t sequence);

/* Recalcula el payload de un mensaje extraido y dice si coincide. Esto ES
 * la columna payload_ok de D12; la llama el consumidor (#15) despues de
 * sync_get, fuera de la region critica.
 *
 * Tres comparaciones, con el SOBRE del mensaje como referencia:
 *
 *   1. identidad: msg->producer_id / msg->sequence contra las copias de
 *      adentro del payload (chequeo cruzado de D1);
 *   2. relleno: cada byte de fill contra el que corresponde a esa identidad;
 *   3. checksum: el campo guardado contra el recalculado sobre el fill que
 *      venia en el mensaje.
 *
 * Entre las tres cubren los 32 bytes del payload: ninguno puede cambiar sin
 * que esto devuelva false. El (1) solo se puede hacer aca: el CSV de D12
 * tiene una sola columna producer_id y una sola sequence, asi que si el
 * sobre y el payload discrepan, el verificador (#7) ya no ve la diferencia.
 *
 * Devuelve un bool y no el motivo porque D12 define payload_ok como un bit;
 * si alguna vez hace falta el motivo, los tres chequeos son separables. */
bool payload_verify(const struct message *msg);

#endif /* SOA_P2_PAYLOAD_H */
