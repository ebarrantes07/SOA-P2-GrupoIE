/* clock.h — reloj monotonico compartido por todo el proyecto.
 *
 * Centraliza clock_gettime(CLOCK_MONOTONIC, ...) en un solo lugar: D3, D10
 * y D11 lo usan para marcas de tiempo (enqueue_time_ns, closing_start_ns,
 * snapshots de metricas, etc.) y todos deben leer del mismo reloj.
 */
#ifndef SOA_P2_CLOCK_H
#define SOA_P2_CLOCK_H

#include <stdint.h>

/* Marca actual del reloj monotonico, en nanosegundos desde un punto de
 * referencia no especificado pero compartido entre todos los procesos de
 * esta maquina (no sobrevive un reinicio, no es comparable entre maquinas
 * distintas). Por eso D3/D11 pueden restar una marca tomada por el
 * productor de una tomada despues por el consumidor, aunque sean procesos
 * distintos: ambas vienen del mismo reloj de sistema. */
uint64_t now_monotonic_ns(void);

#endif /* SOA_P2_CLOCK_H */
