/* test_cli.c — pruebas del análisis de argumentos (D15, Issue #4). Cubren
 * los rechazos del caso "Validación" del enunciado que son de la CLI:
 * argumentos ausentes, capacidad 0, valores no numéricos, fuera de rango y
 * overflow. El id duplicado y la instancia inexistente se detectan al
 * conectarse (#12, memory_open) y se prueban en #19.
 */
#include "cli.h"
#include "test_util.h"

#include <stdarg.h>
#include <string.h>

static char err[256];

/* Arma argv con los argumentos (terminados en NULL) y analiza. */
static enum cli_result run(enum cli_role role, struct cli_options *o, ...) {
    char *argv[32] = {"prog"};
    int argc = 1;
    va_list ap;
    va_start(ap, o);
    for (char *a; (a = va_arg(ap, char *)) != NULL && argc < 31;)
        argv[argc++] = a;
    va_end(ap);
    err[0] = '\0';
    return cli_parse(role, argc, argv, o, err, sizeof(err));
}

/* Rechazo cuyo mensaje contiene `expect`. */
#define REJECTS(expect, role, ...) \
    do { \
        struct cli_options o_; \
        CHECK(run(role, &o_, __VA_ARGS__, (char *)NULL) == CLI_ERROR); \
        CHECK(strstr(err, expect) != NULL); \
    } while (0)

static bool test_creator_valid(void) {
    struct cli_options o;
    CHECK(run(CLI_CREATOR, &o, "--name", "/soa_p2_g1", "--capacity", "8", NULL) == CLI_OK);
    CHECK(strcmp(o.name, "/soa_p2_g1") == 0);
    CHECK(o.capacity == 8);
    /* el orden de las opciones no importa */
    CHECK(run(CLI_CREATOR, &o, "--capacity", "1024", "--name", "/x", NULL) == CLI_OK);
    CHECK(o.capacity == 1024);
    CHECK(run(CLI_CREATOR, &o, "--capacity", "1", "--name", "/x", NULL) == CLI_OK);
    return true;
}

static bool test_missing_arguments(void) {
    struct cli_options o;
    CHECK(run(CLI_CREATOR, &o, NULL) == CLI_ERROR);
    CHECK(strstr(err, "falta la opcion obligatoria --name") != NULL);
    REJECTS("falta la opcion obligatoria --capacity", CLI_CREATOR, "--name", "/x");
    REJECTS("falta la opcion obligatoria --name", CLI_CREATOR, "--capacity", "8");
    REJECTS("falta la opcion obligatoria --log", CLI_PRODUCER,
            "--name", "/x", "--id", "1", "--mean-ms", "5", "--seed", "21");
    REJECTS("falta la opcion obligatoria --seed", CLI_CONSUMER,
            "--name", "/x", "--id", "1", "--mean-ms", "5", "--log", "c.csv");
    REJECTS("falta la opcion obligatoria --summary", CLI_FINISHER, "--name", "/x");
    return true;
}

static bool test_missing_value(void) {
    REJECTS("falta el valor de --capacity", CLI_CREATOR, "--name", "/x", "--capacity");
    /* el valor faltante se nota aunque venga otra opción después */
    REJECTS("falta el valor de --name", CLI_CREATOR, "--name", "--capacity", "8");
    return true;
}

static bool test_capacity_out_of_range(void) {
    REJECTS("--capacity invalida: '0'", CLI_CREATOR, "--name", "/x", "--capacity", "0");
    REJECTS("--capacity invalida: '1025'", CLI_CREATOR, "--name", "/x", "--capacity", "1025");
    return true;
}

static bool test_non_numeric(void) {
    static const char *bad[] = {"abc", "8x", " 8", "8 ", "+8", "-5", "", "0x10", "8.0", "1e3"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        struct cli_options o;
        CHECK(run(CLI_CREATOR, &o, "--name", "/x", "--capacity", bad[i], NULL) == CLI_ERROR);
        CHECK(strstr(err, "--capacity invalida") != NULL);
    }
    REJECTS("--id invalido", CLI_CONSUMER, "--name", "/x", "--id", "uno",
            "--mean-ms", "5", "--seed", "1", "--log", "c.csv");
    return true;
}

static bool test_overflow(void) {
    REJECTS("--capacity invalida", CLI_CREATOR, "--name", "/x",
            "--capacity", "99999999999999999999999");
    REJECTS("--id invalido", CLI_CONSUMER, "--name", "/x", "--id", "4294967296",
            "--mean-ms", "5", "--seed", "1", "--log", "c.csv");
    REJECTS("--seed invalida", CLI_CONSUMER, "--name", "/x", "--id", "1",
            "--mean-ms", "5", "--seed", "4294967296", "--log", "c.csv");
    REJECTS("--messages invalido", CLI_PRODUCER, "--name", "/x", "--id", "1",
            "--mean-ms", "5", "--seed", "1", "--log", "p.csv",
            "--messages", "18446744073709551616");   /* UINT64_MAX + 1 */
    return true;
}

static bool test_u32_u64_limits(void) {
    uint32_t v32;
    uint64_t v64;
    CHECK(cli_parse_u32("4294967295", 1, UINT32_MAX, &v32) && v32 == UINT32_MAX);
    CHECK(!cli_parse_u32("4294967296", 1, UINT32_MAX, &v32));
    CHECK(!cli_parse_u32("0", 1, UINT32_MAX, &v32));
    CHECK(cli_parse_u64("18446744073709551615", 1, UINT64_MAX, &v64) && v64 == UINT64_MAX);
    CHECK(!cli_parse_u64("18446744073709551616", 1, UINT64_MAX, &v64));
    CHECK(cli_parse_u32("007", 1, 10, &v32) && v32 == 7);   /* ceros a la izquierda: decimal */
    return true;
}

static bool test_seed_and_id_zero(void) {
    REJECTS("0 no sirve para xorshift32", CLI_PRODUCER, "--name", "/x", "--id", "1",
            "--mean-ms", "5", "--seed", "0", "--log", "p.csv");
    REJECTS("--id invalido: '0'", CLI_PRODUCER, "--name", "/x", "--id", "0",
            "--mean-ms", "5", "--seed", "1", "--log", "p.csv");
    return true;
}

static bool test_mean_ms(void) {
    double v;
    CHECK(cli_parse_mean_ms("0", &v) && v == 0.0);
    CHECK(cli_parse_mean_ms("1000", &v) && v == 1000.0);
    CHECK(cli_parse_mean_ms("2.5", &v) && v == 2.5);
    CHECK(cli_parse_mean_ms("1000.0", &v) && v == 1000.0);
    static const char *bad[] = {"1000.5", "1001", "-1", "+5", "inf", "nan", "NaN",
                                "1e2", "0x10", ".5", "5.", "", " 5", "5ms"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(!cli_parse_mean_ms(bad[i], &v));
    return true;
}

static bool test_messages_optional(void) {
    struct cli_options o;
    CHECK(run(CLI_PRODUCER, &o, "--name", "/x", "--id", "1", "--mean-ms", "5",
              "--seed", "21", "--log", "p.csv", NULL) == CLI_OK);
    CHECK(!o.has_messages);   /* sin cuota: sigue hasta el cierre */
    CHECK(run(CLI_PRODUCER, &o, "--name", "/x", "--id", "1", "--mean-ms", "5",
              "--seed", "21", "--log", "p.csv", "--messages", "1000", NULL) == CLI_OK);
    CHECK(o.has_messages && o.messages == 1000);
    REJECTS("--messages invalido: '0'", CLI_PRODUCER, "--name", "/x", "--id", "1",
            "--mean-ms", "5", "--seed", "21", "--log", "p.csv", "--messages", "0");
    return true;
}

static bool test_interface_examples(void) {
    /* Los cuatro comandos de ejemplo del enunciado. */
    struct cli_options o;
    CHECK(run(CLI_CREATOR, &o, "--name", "/soa_p2_g1", "--capacity", "8", NULL) == CLI_OK);
    CHECK(run(CLI_CONSUMER, &o, "--name", "/soa_p2_g1", "--id", "1", "--mean-ms", "10",
              "--seed", "11", "--log", "results/c1.csv", NULL) == CLI_OK);
    CHECK(o.id == 1 && o.mean_ms == 10.0 && o.seed == 11);
    CHECK(strcmp(o.log_path, "results/c1.csv") == 0);
    CHECK(run(CLI_PRODUCER, &o, "--name", "/soa_p2_g1", "--id", "1", "--mean-ms", "5",
              "--seed", "21", "--messages", "1000", "--log", "results/p1.csv", NULL) == CLI_OK);
    CHECK(o.messages == 1000 && o.seed == 21);
    CHECK(run(CLI_FINISHER, &o, "--name", "/soa_p2_g1", "--summary", "results/resumen.csv",
              NULL) == CLI_OK);
    CHECK(o.log_path == NULL);
    CHECK(run(CLI_FINISHER, &o, "--name", "/x", "--summary", "r.csv", "--log", "f.csv",
              NULL) == CLI_OK);
    CHECK(strcmp(o.log_path, "f.csv") == 0);
    return true;
}

static bool test_unknown_repeated_and_wrong_role(void) {
    REJECTS("opcion desconocida: '--foo'", CLI_CREATOR, "--foo");
    REJECTS("opcion desconocida: 'extra'", CLI_CREATOR, "--name", "/x", "--capacity", "8", "extra");
    REJECTS("opcion repetida: --capacity", CLI_CREATOR,
            "--name", "/x", "--capacity", "8", "--capacity", "9");
    REJECTS("la opcion --capacity no aplica a este ejecutable", CLI_PRODUCER, "--capacity", "8");
    REJECTS("la opcion --messages no aplica a este ejecutable", CLI_CONSUMER, "--messages", "5");
    REJECTS("la opcion --id no aplica a este ejecutable", CLI_CREATOR, "--id", "1");
    return true;
}

static bool test_name_and_paths(void) {
    REJECTS("--name invalido: 'sin_barra'", CLI_CREATOR, "--name", "sin_barra", "--capacity", "8");
    REJECTS("--name invalido", CLI_CREATOR, "--name", "/a/b", "--capacity", "8");
    REJECTS("--log no puede ser una ruta vacia", CLI_CONSUMER, "--name", "/x", "--id", "1",
            "--mean-ms", "5", "--seed", "1", "--log", "");
    REJECTS("--summary no puede ser una ruta vacia", CLI_FINISHER, "--name", "/x", "--summary", "");
    return true;
}

static bool test_help(void) {
    struct cli_options o;
    CHECK(run(CLI_PRODUCER, &o, "--help", NULL) == CLI_HELP);
    CHECK(run(CLI_CREATOR, &o, "--name", "/x", "--help", NULL) == CLI_HELP);
    return true;
}

int main(void) {
    struct test_case tests[] = {
        {"creator_valid", test_creator_valid},
        {"missing_arguments", test_missing_arguments},
        {"missing_value", test_missing_value},
        {"capacity_out_of_range", test_capacity_out_of_range},
        {"non_numeric", test_non_numeric},
        {"overflow", test_overflow},
        {"u32_u64_limits", test_u32_u64_limits},
        {"seed_and_id_zero", test_seed_and_id_zero},
        {"mean_ms", test_mean_ms},
        {"messages_optional", test_messages_optional},
        {"interface_examples", test_interface_examples},
        {"unknown_repeated_and_wrong_role", test_unknown_repeated_and_wrong_role},
        {"name_and_paths", test_name_and_paths},
        {"help", test_help},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
