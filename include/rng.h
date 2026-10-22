/* rng.h — generador xorshift32 y muestreo exponencial para las pausas.
 *
 * Mismo generador que el Proyecto 1 (ver SOA-P1-GrupoIE/src/rng.c): estado
 * de 32 bits, sin dependencias externas, un valor por proceso. Se
 * reutiliza la secuencia de actualizacion tal cual; lo que agrega este
 * modulo es el muestreo exponencial que no existia en el P1.
 */
#ifndef SOA_P2_RNG_H
#define SOA_P2_RNG_H

#include <stdint.h>

struct rng {
    uint32_t state;
};

/* Inicializa rng con la semilla dada.
 *
 * Precondicion de uso: seed != 0 -- con estado 0, xorshift32 queda
 * atascado generando siempre 0 (0 es el unico punto fijo de la
 * transformacion). Por eso --seed 0 debe rechazarse en la CLI (#4), antes
 * de que este codigo se llame; esta funcion no sustituye esa validacion,
 * solo se niega a inicializar con un estado que no sirve.
 *
 * Retorna 0 en exito, -1 si seed == 0 (rng no se modifica).
 */
int rng_init(struct rng *rng, uint32_t seed);

/* Avanza el estado con xorshift32 (x^=x<<13; x^=x>>17; x^=x<<5;) y
 * devuelve el nuevo valor. Con una semilla != 0, nunca devuelve 0: es el
 * unico valor que no aparece en el ciclo. */
uint32_t rng_next(struct rng *rng);

/* Uniforme en (0, 1]: nunca da exactamente 0 (evita log(0)). Puede dar
 * exactamente 1.0 cuando rng_next() devuelve su valor maximo. */
double rng_uniform01(struct rng *rng);

/* Duracion en milisegundos de una pausa exponencial de media mean_ms,
 * por transformada inversa: -mean_ms * ln(U), con U = rng_uniform01().
 *
 * Precondicion: mean_ms > 0. El enunciado dice que --mean-ms 0 desactiva
 * las pausas "sin llamar a la funcion de muestreo" -- esa decision es del
 * llamador (#14/#15), no de esta funcion.
 */
double rng_exponential_ms(struct rng *rng, double mean_ms);

#endif /* SOA_P2_RNG_H */
