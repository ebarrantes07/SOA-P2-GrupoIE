/* metricas.h — métricas locales por proceso, agregados compartidos de la
 * instancia, histograma de latencia y resumen del finisher.
 *
 * Decisiones documentadas en docs/decisiones_diseno.md (D11 métricas y
 * resumen, D12 formato de los CSV). No cambiar este archivo sin agregar una
 * entrada nueva ahí explicando por qué.
 *
 * Igual que participantes.h, no incluye memory.h: memory.h lo incluye a él
 * para embeber struct instance_metrics en el segmento.
 */
#ifndef SOA_P2_METRICAS_H
#define SOA_P2_METRICAS_H

#include <stddef.h>
#include <stdint.h>

struct instance;            /* definida en memory.h */

/* ---- Histograma de latencia (vive en el segmento) --------------------- */

/* Log-lineal: 8 sub-cubetas por potencia de 2. Valores < 8 ns son exactos;
 * el resto cae en una cubeta de ancho 2^(e-3), así el error relativo de un
 * percentil reportado en el punto medio de la cubeta es <= 6.25 %. Cubre
 * todo uint64_t. Ver D11. */
#define LAT_SUB_BITS     3u
#define LAT_SUB_BUCKETS  (1u << LAT_SUB_BITS)                   /* 8 */
#define LAT_BUCKETS      ((64u - LAT_SUB_BITS + 1u) * LAT_SUB_BUCKETS)  /* 496 */

struct latency_hist {
    uint64_t count;
    uint64_t sum_ns;
    uint64_t max_ns;
    uint64_t buckets[LAT_BUCKETS];
};

/* Agregados de la instancia. Todos se actualizan bajo el mutex. */
struct instance_metrics {
    uint64_t first_start_ns;     /* registro del primer trabajador (0 = ninguno aún) */
    uint64_t last_dequeue_ns;    /* marca del último get exitoso */
    uint64_t closing_start_ns;   /* transición OPEN -> CLOSING */
    uint64_t rejected_puts;      /* intentos rechazados por el corte, no son aceptados */
    struct latency_hist latency; /* enqueue_time_ns -> extracción, en ns */
};

/* ---- Métricas locales de un proceso ----------------------------------- */

/* Las acumula cada productor/consumidor en su memoria privada a partir de
 * lo que devuelve struct op_snapshot; se copian a su participant_slot al
 * retirarse (participant_leave). */
struct participant_stats {
    uint64_t ops;            /* aceptados (prod.) o extraídos (cons.) */
    uint64_t wait_ns;        /* espera en not_full (prod.) o not_empty (cons.) */
    uint64_t wait_count;
    uint64_t lock_ns;        /* adquisición del mutex en put/get */
};

/* ---- Histograma -------------------------------------------------------- */

/* Índice de cubeta para un valor; total y determinista. */
uint32_t lat_hist_bucket(uint64_t ns);

/* Punto medio (en ns) del rango que cubre una cubeta. */
uint64_t lat_hist_bucket_mid(uint32_t bucket);

/* Agrega una muestra. Precondición: mutex de la instancia tomado (la llama
 * sync_get al extraer; es O(1)). */
void lat_hist_add(struct latency_hist *h, uint64_t ns);

/* Media exacta (sum/count) y percentil aproximado p en (0, 100], en ns.
 * Con count == 0 ambas devuelven 0. */
double   lat_hist_mean_ns(const struct latency_hist *h);
uint64_t lat_hist_percentile_ns(const struct latency_hist *h, double p);

/* ---- Estadística entre repeticiones (experimento, scripts) ------------ */

/* Media y desviación estándar muestral (n - 1). Con n < 2 la desviación
 * es 0. Precondición: values != NULL si n > 0. */
double stats_mean(const double *values, size_t n);
double stats_stddev(const double *values, size_t n);

/* Mensajes extraídos por segundo entre el primer inicio de un trabajador y
 * la última extracción. Devuelve 0 si el intervalo es 0. */
double throughput_msgs_per_s(uint64_t extracted, uint64_t first_start_ns,
                             uint64_t last_dequeue_ns);

/* ---- Resumen del finisher --------------------------------------------- */

/* Escribe el CSV de resumen (formato en D12) a partir del segmento y de la
 * duración del cierre medida por el finisher. Precondición: ya nadie más
 * está conectado (closing_wait_all_left retornó), así que se lee sin
 * mutex. Devuelve 0 o -errno. */
int summary_write_csv(const char *path, const struct instance *inst,
                      uint64_t close_duration_ns);

#endif /* SOA_P2_METRICAS_H */
