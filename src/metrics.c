/* metrics.c — histograma de latencia, estadistica entre repeticiones,
 * throughput y escritor del resumen del finisher (D11, D28).
 */
#include "metrics.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>

#include "memory.h"       /* struct instance; metrics.h no lo incluye (D11) */
#include "participants.h"

/* floor(log2(n)) para n >= 1, con un ciclo portable (sin builtins de
 * compilador). Se llama una vez por extraccion, dentro de la region
 * critica de sync_get: el costo de hasta 64 iteraciones es despreciable
 * frente a la adquisicion del mutex (ver D11). */
static uint32_t floor_log2_u64(uint64_t n) {
    uint32_t e = 0;
    while (n > 1) {
        n >>= 1;
        e++;
    }
    return e;
}

uint32_t lat_hist_bucket(uint64_t ns) {
    if (ns < LAT_SUB_BUCKETS) {   /* ns < 8: una cubeta exacta por valor */
        return (uint32_t)ns;
    }

    uint32_t e = floor_log2_u64(ns);   /* e >= 3, porque ns >= 8 */
    assert(e >= LAT_SUB_BITS);   /* si no, el shift de abajo es UB (negativo) */

    uint32_t sub = (uint32_t)((ns >> (e - LAT_SUB_BITS)) & (LAT_SUB_BUCKETS - 1));
    uint32_t bucket = (e - 2u) * LAT_SUB_BUCKETS + sub;

    /* bucket alimenta directo un indice de arreglo en memoria compartida
     * (h->buckets[...] en lat_hist_add); fuera de rango ahi no crashea
     * limpio, corrompe en silencio lo que este al lado. */
    assert(bucket < LAT_BUCKETS);
    return bucket;
}

uint64_t lat_hist_bucket_mid(uint32_t bucket) {
    assert(bucket < LAT_BUCKETS);

    if (bucket < LAT_SUB_BUCKETS) {   /* las primeras 8 son exactas */
        return bucket;
    }

    uint32_t group = bucket / LAT_SUB_BUCKETS;   /* = e - 2 */
    uint32_t sub = bucket % LAT_SUB_BUCKETS;
    uint32_t e = group + 2u;

    uint64_t width = 1ull << (e - LAT_SUB_BITS);     /* 2^(e-3) */
    uint64_t lo = (1ull << e) + (uint64_t)sub * width;
    return lo + width / 2;   /* punto medio del rango; width siempre par o 1 */
}

void lat_hist_add(struct latency_hist *h, uint64_t ns) {
    h->count++;
    h->sum_ns += ns;
    if (ns > h->max_ns) {
        h->max_ns = ns;
    }
    h->buckets[lat_hist_bucket(ns)]++;
}

double lat_hist_mean_ns(const struct latency_hist *h) {
    if (h->count == 0) {
        return 0.0;
    }
    return (double)h->sum_ns / (double)h->count;
}

uint64_t lat_hist_percentile_ns(const struct latency_hist *h, double p) {
    assert(p > 0.0 && p <= 100.0);

    if (h->count == 0) {
        return 0;
    }

    uint64_t target = (uint64_t)ceil((p / 100.0) * (double)h->count);
    if (target < 1) {
        target = 1;
    }
    if (target > h->count) {
        target = h->count;
    }

    uint64_t cumulative = 0;
    for (uint32_t b = 0; b < LAT_BUCKETS; b++) {
        cumulative += h->buckets[b];
        if (cumulative >= target) {
            return lat_hist_bucket_mid(b);
        }
    }

    /* Inalcanzable si h->count > 0 y las cubetas suman h->count. */
    return h->max_ns;
}

double stats_mean(const double *values, size_t n) {
    assert(n == 0 || values != NULL);

    if (n == 0) {
        return 0.0;
    }

    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        sum += values[i];
    }
    return sum / (double)n;
}

double stats_stddev(const double *values, size_t n) {
    assert(n == 0 || values != NULL);

    if (n < 2) {
        return 0.0;
    }

    double mean = stats_mean(values, n);
    double sum_sq_diff = 0.0;
    for (size_t i = 0; i < n; i++) {
        double diff = values[i] - mean;
        sum_sq_diff += diff * diff;
    }
    return sqrt(sum_sq_diff / (double)(n - 1));   /* muestral: n-1, no n */
}

double throughput_msgs_per_s(uint64_t extracted, uint64_t first_start_ns,
                             uint64_t last_dequeue_ns) {
    if (last_dequeue_ns <= first_start_ns) {
        return 0.0;   /* intervalo invalido o nulo: sin esto, la resta de
                        * enteros sin signo "envolveria" a un numero enorme */
    }

    uint64_t interval_ns = last_dequeue_ns - first_start_ns;
    return (double)extracted * 1e9 / (double)interval_ns;
}


/* ---- Resumen del finisher (D11, #18) ---------------------------------- */

/* Formato largo de D11: una metrica por fila, `metric,role,id,value,unit,
 * note`. El CSV no lleva comillas (D12), asi que ninguna nota puede
 * contener comas; las notas de abajo usan punto y coma. */
static void row_u64(FILE *f, const char *metric, const char *role, uint32_t id,
                    uint64_t value, const char *unit, const char *note) {
    fprintf(f, "%s,%s,%" PRIu32 ",%" PRIu64 ",%s,%s\n",
            metric, role, id, value, unit, note == NULL ? "" : note);
}

/* Hasta 6 decimales, como fija D11. */
static void row_f64(FILE *f, const char *metric, const char *role, uint32_t id,
                    double value, const char *unit, const char *note) {
    fprintf(f, "%s,%s,%" PRIu32 ",%.6f,%s,%s\n",
            metric, role, id, value, unit, note == NULL ? "" : note);
}

/* Una fila por casilla usada. Las esperas se declaran por lo que de verdad
 * contienen, que es el tercer criterio del #18: ninguna se presenta como
 * exclusiva de una causa si combina varias. */
static void participant_rows(FILE *f, const struct participant_table *t,
                             enum role role) {
    const bool prod = (role == ROLE_PRODUCER);
    const char *role_name = prod ? "producer" : "consumer";
    const char *wait_metric = prod ? "wait_space" : "wait_data";
    const char *wait_note = prod
        ? "espera por espacio en not_full; no incluye la pausa simulada"
        : "espera por datos en not_empty; incluye la espera final hasta el cierre";

    for (uint32_t i = 0; i < t->used; i++) {
        const struct participant_slot *s = &t->slots[i];
        row_u64(f, "ops", role_name, s->id, s->ops, "msgs",
                prod ? "aceptados" : "extraidos");
        row_u64(f, wait_metric, role_name, s->id, s->wait_ns, "ns", wait_note);
        row_u64(f, "wait_count", role_name, s->id, s->wait_count, "count", NULL);
        row_u64(f, "lock_acquire", role_name, s->id, s->lock_ns, "ns",
                "incluye la readquisicion al despertar de cond_wait");
        /* Un participante que no se retiro no tiene left_ns: informar
         * left_ns - joined_ns daria un numero enorme al envolver. */
        if (s->state == SLOT_LEFT && s->left_ns >= s->joined_ns)
            row_u64(f, "lifetime", role_name, s->id, s->left_ns - s->joined_ns, "ns", NULL);
        else
            row_u64(f, "lifetime", role_name, s->id, 0, "ns",
                    "no se retiro; sin duracion valida");
    }
}

int summary_write_csv(const char *path, const struct instance *inst,
                      uint64_t close_duration_ns) {
    FILE *f = fopen(path, "w");
    if (f == NULL)
        return -errno;

    fprintf(f, "metric,role,id,value,unit,note\n");

    /* Se lee sin mutex: la precondicion es que ya no queda nadie conectado
     * (closing_wait_all_left retorno), asi que nada de esto cambia. */
    const uint64_t accepted = inst->enqueue_seq;
    const uint64_t extracted = inst->dequeue_seq;
    const uint32_t pending = inst->count;
    const struct instance_metrics *m = &inst->metrics;

    row_u64(f, "accepted", "instance", 0, accepted, "msgs", NULL);
    row_u64(f, "extracted", "instance", 0, extracted, "msgs", NULL);
    row_u64(f, "pending", "instance", 0, pending, "msgs", NULL);
    row_u64(f, "rejected_puts", "instance", 0, m->rejected_puts, "count",
            "intentos rechazados por el corte; no son aceptados");
    row_u64(f, "invariant_ok", "instance", 0,
            (accepted == extracted + pending && pending == 0) ? 1u : 0u, "bool",
            "1 si accepted = extracted + pending y pending = 0");

    row_f64(f, "latency_mean", "instance", 0, lat_hist_mean_ns(&m->latency), "ns",
            "exacta");
    /* 95 y no 0.95: lat_hist_percentile_ns toma el percentil en por
     * ciento. Pasar la fraccion daria casi el minimo sin avisar. */
    row_u64(f, "latency_p95", "instance", 0,
            lat_hist_percentile_ns(&m->latency, 95.0), "ns",
            "aproximada por histograma; error <= 6.25 %");
    row_u64(f, "latency_max", "instance", 0, m->latency.max_ns, "ns", NULL);

    row_f64(f, "throughput", "instance", 0,
            throughput_msgs_per_s(extracted, m->first_start_ns, m->last_dequeue_ns),
            "msgs_per_s", "primer inicio de trabajador a ultima extraccion");
    row_u64(f, "active_interval", "instance", 0,
            m->last_dequeue_ns > m->first_start_ns
                ? m->last_dequeue_ns - m->first_start_ns : 0, "ns",
            "denominador del throughput");
    row_u64(f, "close_duration", "instance", 0, close_duration_ns, "ns",
            "de CLOSING a liberacion completa");

    row_u64(f, "producers", "instance", 0, inst->producers.used, "count",
            "registros de por vida");
    row_u64(f, "consumers", "instance", 0, inst->consumers.used, "count",
            "registros de por vida");

    participant_rows(f, &inst->producers, ROLE_PRODUCER);
    participant_rows(f, &inst->consumers, ROLE_CONSUMER);

    /* ferror antes de fclose: un error de escritura diferida aparece recien
     * al vaciar el buffer. */
    int rc = ferror(f) ? -EIO : 0;
    if (fclose(f) != 0 && rc == 0)
        rc = -errno;
    return rc;
}
