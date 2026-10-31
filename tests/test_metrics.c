/* test_metrics.c — pruebas del histograma de latencia, la estadística
 * (Issue #8) y el resumen del finisher (Issue #18). Datos de entrada
 * conocidos, resultado calculado a mano; sin estadística con tolerancia.
 */
#include "metrics.h"
#include "test_util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "memory.h"
#include "participants.h"

static bool test_bucket_boundaries(void) {
    /* Derivados a mano (ver docs/decisiones_diseno.md, D11). */
    CHECK(lat_hist_bucket(7) == 7);     /* ultima cubeta exacta */
    CHECK(lat_hist_bucket(8) == 8);     /* primer valor del grupo e=3 */
    CHECK(lat_hist_bucket(15) == 15);   /* ultimo del grupo e=3 */
    CHECK(lat_hist_bucket(16) == 16);   /* primer valor del grupo e=4, ancho 2 */
    CHECK(lat_hist_bucket(17) == 16);   /* mismo sub-cajon que 16 */
    CHECK(lat_hist_bucket(31) == 23);   /* ultimo del grupo e=4 */
    CHECK(lat_hist_bucket(UINT64_MAX) == LAT_BUCKETS - 1);
    return true;
}

static bool test_bucket_mid_error_bound(void) {
    /* Para cada ns, el punto medio de su cubeta no se aleja mas del
     * 6.25% documentado. Muestras fijas, no aleatorias. */
    static const uint64_t samples[] = {100, 1000, 1000000, 5000000000ull};
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        uint64_t ns = samples[i];
        uint64_t mid = lat_hist_bucket_mid(lat_hist_bucket(ns));
        uint64_t diff = mid > ns ? mid - ns : ns - mid;
        /* diff / ns <= 0.0625  <=>  diff * 16 <= ns * 1  (evita division) */
        CHECK(diff * 16 <= ns);
    }
    return true;
}

static bool test_hist_add_and_mean(void) {
    struct latency_hist h = {0};
    lat_hist_add(&h, 2);
    lat_hist_add(&h, 4);
    lat_hist_add(&h, 6);
    CHECK(h.count == 3);
    CHECK(h.sum_ns == 12);
    CHECK(h.max_ns == 6);
    CHECK(lat_hist_mean_ns(&h) == 4.0);   /* (2+4+6)/3 */
    return true;
}

static bool test_percentile_known_values(void) {
    struct latency_hist h = {0};
    for (uint64_t v = 1; v <= 7; v++) {
        lat_hist_add(&h, v);   /* 1..7, todas en cubetas exactas */
    }
    CHECK(lat_hist_percentile_ns(&h, 50.0) == 4);    /* ceil(0.5*7)=4to valor */
    CHECK(lat_hist_percentile_ns(&h, 100.0) == 7);
    return true;
}

static bool test_percentile_empty(void) {
    struct latency_hist h = {0};
    CHECK(lat_hist_percentile_ns(&h, 95.0) == 0);
    CHECK(lat_hist_mean_ns(&h) == 0.0);
    return true;
}

static bool test_stats_mean_stddev(void) {
    double vals[] = {2.0, 4.0, 6.0};
    /* a mano: mean=4; varianza muestral = ((2-4)^2+(4-4)^2+(6-4)^2)/(3-1) = 4; sd=2 */
    CHECK(stats_mean(vals, 3) == 4.0);
    CHECK(stats_stddev(vals, 3) == 2.0);
    return true;
}

static bool test_stats_edge_cases(void) {
    double one[] = {5.0};
    CHECK(stats_stddev(one, 1) == 0.0);
    CHECK(stats_mean(NULL, 0) == 0.0);
    CHECK(stats_stddev(NULL, 0) == 0.0);
    return true;
}

static bool test_throughput(void) {
    /* 100 mensajes en 1 segundo exacto */
    CHECK(throughput_msgs_per_s(100, 0, 1000000000ull) == 100.0);
    /* intervalo nulo */
    CHECK(throughput_msgs_per_s(50, 1000000000ull, 1000000000ull) == 0.0);
    /* orden invertido: NO debe envolver a un numero gigante */
    CHECK(throughput_msgs_per_s(50, 2000000000ull, 1000000000ull) == 0.0);
    return true;
}


/* ---- Resumen del finisher (#18) --------------------------------------- */

#define MAX_SUMMARY_ROWS 128

struct summary_row {
    char metric[32], role[16], unit[16], note[96];
    uint32_t id;
    double value;
};

struct summary {
    struct summary_row rows[MAX_SUMMARY_ROWS];
    size_t n;
    unsigned bad_field_count;    /* filas que no traen 6 campos */
};

/* Lee el resumen contando los campos de cada fila: una nota con una coma
 * romperia el CSV sin que nada mas lo note (D12 no usa comillas). */
static bool summary_load(const char *path, struct summary *out) {
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return false;
    char line[512];
    bool header = true;
    while (fgets(line, sizeof(line), f) != NULL) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[--len] = '\0';
        unsigned commas = 0;
        for (size_t i = 0; i < len; i++)
            if (line[i] == ',')
                commas++;
        if (commas != 5)
            out->bad_field_count++;
        if (header) {
            header = false;
            if (strcmp(line, "metric,role,id,value,unit,note") != 0)
                out->bad_field_count++;
            continue;
        }
        if (out->n == MAX_SUMMARY_ROWS)
            break;
        struct summary_row *r = &out->rows[out->n];
        char *save = line, *tok;
        const char *fields[6] = {0};
        for (unsigned i = 0; i < 6; i++) {
            tok = save;
            char *comma = strchr(save, ',');
            if (comma != NULL) {
                *comma = '\0';
                save = comma + 1;
            } else {
                save = save + strlen(save);
            }
            fields[i] = tok;
        }
        snprintf(r->metric, sizeof(r->metric), "%s", fields[0]);
        snprintf(r->role, sizeof(r->role), "%s", fields[1]);
        r->id = (uint32_t)strtoul(fields[2], NULL, 10);
        r->value = strtod(fields[3], NULL);
        snprintf(r->unit, sizeof(r->unit), "%s", fields[4]);
        snprintf(r->note, sizeof(r->note), "%s", fields[5]);
        out->n++;
    }
    fclose(f);
    return true;
}

static const struct summary_row *find_row(const struct summary *s, const char *metric,
                                          const char *role, uint32_t id) {
    for (size_t i = 0; i < s->n; i++)
        if (strcmp(s->rows[i].metric, metric) == 0 &&
            strcmp(s->rows[i].role, role) == 0 && s->rows[i].id == id)
            return &s->rows[i];
    return NULL;
}

static double row_value(const struct summary *s, const char *metric) {
    const struct summary_row *r = find_row(s, metric, "instance", 0);
    return r == NULL ? -1.0 : r->value;
}

static const char *sum_path(void) {
    static char buf[80];
    snprintf(buf, sizeof(buf), "build/t18_%d_resumen.csv", (int)getpid());
    return buf;
}

static const char *inst_name(const char *tag) {
    static char buf[INSTANCE_NAME_MAX + 1];
    snprintf(buf, sizeof(buf), "/t18_%d_%s", (int)getpid(), tag);
    memory_unlink(buf);
    return buf;
}

/* Una instancia con dos productores y dos consumidores ya retirados, con
 * metricas conocidas, lista para resumir. */
static bool build_instance(const char *name, struct instance **inst, size_t *len) {
    if (memory_create(name, 8, inst, len) != MEM_OK)
        return false;
    struct instance *i = *inst;
    i->enqueue_seq = 100;
    i->dequeue_seq = 100;
    i->count = 0;
    i->metrics.rejected_puts = 3;
    i->metrics.first_start_ns = 1000000000ull;          /* 1 s */
    i->metrics.last_dequeue_ns = 3000000000ull;         /* 3 s -> intervalo 2 s */
    for (uint64_t v = 1; v <= 100; v++)
        lat_hist_add(&i->metrics.latency, v * 1000000ull);   /* 1..100 ms */

    int slot;
    struct participant_stats ps = { .ops = 60, .wait_ns = 7000, .wait_count = 5, .lock_ns = 900 };
    if (participant_register(i, ROLE_PRODUCER, 11, &slot) != REG_OK)
        return false;
    participant_leave(i, ROLE_PRODUCER, slot, &ps);
    ps.ops = 40;
    if (participant_register(i, ROLE_PRODUCER, 12, &slot) != REG_OK)
        return false;
    participant_leave(i, ROLE_PRODUCER, slot, &ps);
    ps.ops = 55;
    if (participant_register(i, ROLE_CONSUMER, 21, &slot) != REG_OK)
        return false;
    participant_leave(i, ROLE_CONSUMER, slot, &ps);
    ps.ops = 45;
    if (participant_register(i, ROLE_CONSUMER, 22, &slot) != REG_OK)
        return false;
    participant_leave(i, ROLE_CONSUMER, slot, &ps);
    return true;
}

static bool test_summary_instance_rows(void) {
    const char *name = inst_name("inst");
    struct instance *inst;
    size_t len;
    CHECK(build_instance(name, &inst, &len));
    CHECK(summary_write_csv(sum_path(), inst, 450000) == 0);

    struct summary s;
    CHECK(summary_load(sum_path(), &s));
    CHECK(s.bad_field_count == 0);          /* cabecera y 6 campos por fila */

    CHECK(row_value(&s, "accepted") == 100.0);
    CHECK(row_value(&s, "extracted") == 100.0);
    CHECK(row_value(&s, "pending") == 0.0);
    CHECK(row_value(&s, "rejected_puts") == 3.0);
    CHECK(row_value(&s, "invariant_ok") == 1.0);
    /* media de 1..100 ms = 50.5 ms */
    CHECK(row_value(&s, "latency_mean") > 50400000.0 && row_value(&s, "latency_mean") < 50600000.0);
    CHECK(row_value(&s, "latency_max") == 100000000.0);
    /* 100 extraidos en 2 s = 50 msgs/s */
    CHECK(row_value(&s, "throughput") == 50.0);
    CHECK(row_value(&s, "active_interval") == 2000000000.0);
    CHECK(row_value(&s, "close_duration") == 450000.0);
    CHECK(row_value(&s, "producers") == 2.0);
    CHECK(row_value(&s, "consumers") == 2.0);

    /* Las unidades son las de D11. */
    CHECK(strcmp(find_row(&s, "throughput", "instance", 0)->unit, "msgs_per_s") == 0);
    CHECK(strcmp(find_row(&s, "invariant_ok", "instance", 0)->unit, "bool") == 0);

    unlink(sum_path());
    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* El p95 se pide en POR CIENTO. Pasar 0.95 por error daria casi el minimo
 * (el primer milisegundo) y nada se quejaria: con 1..100 ms, el p95 tiene
 * que caer cerca de 95 ms y no cerca de 1 ms. */
static bool test_summary_p95_is_percent(void) {
    const char *name = inst_name("p95");
    struct instance *inst;
    size_t len;
    CHECK(build_instance(name, &inst, &len));
    CHECK(summary_write_csv(sum_path(), inst, 0) == 0);
    struct summary s;
    CHECK(summary_load(sum_path(), &s));
    double p95 = row_value(&s, "latency_p95");
    CHECK(p95 > 80000000.0);        /* > 80 ms: no es el minimo */
    CHECK(p95 < 120000000.0);       /* y tampoco se fue de rango */
    unlink(sum_path());
    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

static bool test_summary_participant_rows(void) {
    const char *name = inst_name("part");
    struct instance *inst;
    size_t len;
    CHECK(build_instance(name, &inst, &len));
    CHECK(summary_write_csv(sum_path(), inst, 0) == 0);
    struct summary s;
    CHECK(summary_load(sum_path(), &s));

    /* Una fila por metrica y por participante, con su rol y su id. */
    CHECK(find_row(&s, "ops", "producer", 11)->value == 60.0);
    CHECK(find_row(&s, "ops", "producer", 12)->value == 40.0);
    CHECK(find_row(&s, "ops", "consumer", 21)->value == 55.0);
    CHECK(find_row(&s, "ops", "consumer", 22)->value == 45.0);

    /* La metrica de espera se llama distinto segun el rol (D11). */
    CHECK(find_row(&s, "wait_space", "producer", 11) != NULL);
    CHECK(find_row(&s, "wait_data", "producer", 11) == NULL);
    CHECK(find_row(&s, "wait_data", "consumer", 21) != NULL);
    CHECK(find_row(&s, "wait_space", "consumer", 21) == NULL);

    /* La espera del consumidor se declara combinada: tercer criterio del
     * #18. */
    CHECK(strstr(find_row(&s, "wait_data", "consumer", 21)->note, "espera final") != NULL);

    /* La readquisicion tras cond_wait se declara en la metrica de espera
     * de cada rol, y lock_acquire remite a esa misma fila (#61). Que la
     * afirmacion sea cierta lo comprueba put_blocks_and_counts_the_wait en
     * test_sync.c midiendo; aca se verifica que cada rol apunte a la fila
     * que de verdad tiene. */
    CHECK(strstr(find_row(&s, "wait_space", "producer", 11)->note, "readquisicion") != NULL);
    CHECK(strstr(find_row(&s, "wait_data", "consumer", 21)->note, "readquisicion") != NULL);
    CHECK(strstr(find_row(&s, "lock_acquire", "producer", 11)->note, "wait_space") != NULL);
    CHECK(strstr(find_row(&s, "lock_acquire", "consumer", 21)->note, "wait_data") != NULL);

    CHECK(find_row(&s, "wait_count", "producer", 11)->value == 5.0);
    CHECK(find_row(&s, "lock_acquire", "producer", 11)->value == 900.0);
    CHECK(find_row(&s, "lifetime", "consumer", 22) != NULL);

    /* Invariante que el resumen tiene que respetar: lo que extrajeron los
     * consumidores suma los extraidos de la instancia. */
    double sum_ops = find_row(&s, "ops", "consumer", 21)->value +
                     find_row(&s, "ops", "consumer", 22)->value;
    CHECK(sum_ops == row_value(&s, "extracted"));

    unlink(sum_path());
    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* Instancia sin trabajo: ninguna division por cero y nada inventado. */
static bool test_summary_empty_instance(void) {
    const char *name = inst_name("vacia");
    struct instance *inst;
    size_t len;
    CHECK(memory_create(name, 4, &inst, &len) == MEM_OK);
    CHECK(summary_write_csv(sum_path(), inst, 0) == 0);
    struct summary s;
    CHECK(summary_load(sum_path(), &s));
    CHECK(s.bad_field_count == 0);
    CHECK(row_value(&s, "accepted") == 0.0);
    CHECK(row_value(&s, "latency_mean") == 0.0);
    CHECK(row_value(&s, "latency_p95") == 0.0);
    CHECK(row_value(&s, "throughput") == 0.0);
    CHECK(row_value(&s, "active_interval") == 0.0);
    CHECK(row_value(&s, "invariant_ok") == 1.0);     /* 0 = 0 + 0 */
    CHECK(row_value(&s, "producers") == 0.0);
    CHECK(find_row(&s, "ops", "producer", 0) == NULL);   /* sin casillas */
    unlink(sum_path());
    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* Mensajes sin extraer al cerrar: el invariante tiene que dar 0, no
 * esconderse. */
static bool test_summary_invariant_detects_pending(void) {
    const char *name = inst_name("pend");
    struct instance *inst;
    size_t len;
    CHECK(memory_create(name, 8, &inst, &len) == MEM_OK);
    inst->enqueue_seq = 10;
    inst->dequeue_seq = 7;
    inst->count = 3;
    CHECK(summary_write_csv(sum_path(), inst, 0) == 0);
    struct summary s;
    CHECK(summary_load(sum_path(), &s));
    CHECK(row_value(&s, "pending") == 3.0);
    CHECK(row_value(&s, "invariant_ok") == 0.0);
    unlink(sum_path());
    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* Un participante que no se retiro no tiene left_ns: informar
 * left_ns - joined_ns daria un numero enorme al envolver. */
static bool test_summary_slot_without_leave(void) {
    const char *name = inst_name("activo");
    struct instance *inst;
    size_t len;
    CHECK(memory_create(name, 4, &inst, &len) == MEM_OK);
    int slot;
    CHECK(participant_register(inst, ROLE_CONSUMER, 7, &slot) == REG_OK);
    CHECK(summary_write_csv(sum_path(), inst, 0) == 0);
    struct summary s;
    CHECK(summary_load(sum_path(), &s));
    const struct summary_row *lt = find_row(&s, "lifetime", "consumer", 7);
    CHECK(lt != NULL);
    CHECK(lt->value == 0.0);
    CHECK(strstr(lt->note, "no se retiro") != NULL);
    unlink(sum_path());
    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

static bool test_summary_reports_open_error(void) {
    const char *name = inst_name("err");
    struct instance *inst;
    size_t len;
    CHECK(memory_create(name, 4, &inst, &len) == MEM_OK);
    /* Directorio que no existe: el error se propaga como -errno. */
    CHECK(summary_write_csv("build/no_existe_t18/resumen.csv", inst, 0) == -ENOENT);
    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

int main(void) {
    struct test_case tests[] = {
        {"bucket_boundaries", test_bucket_boundaries},
        {"bucket_mid_error_bound", test_bucket_mid_error_bound},
        {"hist_add_and_mean", test_hist_add_and_mean},
        {"percentile_known_values", test_percentile_known_values},
        {"percentile_empty", test_percentile_empty},
        {"stats_mean_stddev", test_stats_mean_stddev},
        {"stats_edge_cases", test_stats_edge_cases},
        {"throughput", test_throughput},
        {"summary_instance_rows", test_summary_instance_rows},
        {"summary_p95_is_percent", test_summary_p95_is_percent},
        {"summary_participant_rows", test_summary_participant_rows},
        {"summary_empty_instance", test_summary_empty_instance},
        {"summary_invariant_detects_pending", test_summary_invariant_detects_pending},
        {"summary_slot_without_leave", test_summary_slot_without_leave},
        {"summary_reports_open_error", test_summary_reports_open_error},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
