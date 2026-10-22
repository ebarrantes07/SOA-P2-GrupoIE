/* test_verify_cli.c — pruebas del ejecutable `verify` sobre los fixtures
 * de tests/fixtures (Issue #7, slice 4).
 *
 * Corre el binario de verdad en vez de llamar a las funciones: es la
 * unica forma de verificar el codigo de salida y el formato del reporte,
 * que son criterios de aceptacion del issue. Depende de que `make test`
 * construya `all` antes de correr las pruebas, y de que el directorio de
 * trabajo sea la raiz del repo (los dos valen con el Makefile actual).
 */
#include "test_util.h"

#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

#define FIXTURES "tests/fixtures/"

/* Corre ./verify con `args`, deja la salida combinada en `out` y devuelve
 * el codigo de salida, o -1 si no se pudo lanzar o no termino normal. */
static int run_verify(const char *args, char *out, size_t out_size) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "./verify %s 2>&1", args);

    FILE *p = popen(cmd, "r");
    if (!p) {
        return -1;
    }

    size_t len = 0;
    int c;
    while ((c = fgetc(p)) != EOF) {
        if (len + 1 < out_size) {
            out[len++] = (char)c;
        }
    }
    out[len] = '\0';

    int rc = pclose(p);
    if (rc == -1 || !WIFEXITED(rc)) {
        return -1;
    }
    return WEXITSTATUS(rc);
}

/* Imprime la salida real cuando algo no coincide: sin esto, un fallo solo
 * dice "status == 1" y no se ve que paso. */
static bool expect(const char *args, int want_status, const char *want_text) {
    char out[8192];
    int status = run_verify(args, out, sizeof(out));
    if (status != want_status ||
        (want_text != NULL && strstr(out, want_text) == NULL)) {
        fprintf(stderr, "\n  verify %s\n  esperaba exit %d y <%s>\n"
                        "  obtuvo exit %d y:\n%s",
                args, want_status, want_text ? want_text : "(nada)",
                status, out);
        return false;
    }
    return true;
}

static bool test_cli_clean_run(void) {
    CHECK(expect(FIXTURES "ok.csv", 0, "sin violaciones"));
    CHECK(expect(FIXTURES "ok.csv", 0, "cierre verificado"));
    return true;
}

/* Un fixture por tipo de violacion: sale 1 y el reporte lo nombra. */
static bool test_cli_each_violation(void) {
    static const struct { const char *file; const char *text; } cases[] = {
        {"perdida.csv",              "mensaje perdido"},
        {"duplicado.csv",            "mensaje duplicado: extraido 2 veces"},
        {"fantasma.csv",             "mensaje fantasma"},
        {"identidad_repetida.csv",   "identidad repetida"},
        {"alterado.csv",             "contenido alterado: 0x00001111 al insertar"},
        {"payload_bad.csv",          "payload_ok = 0"},
        {"fifo.csv",                 "orden FIFO roto"},
        {"put_tardio.csv",           "aceptado despues del corte"},
        {"cierre_inconsistente.csv", "cierre inconsistente"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char args[256];
        snprintf(args, sizeof(args), FIXTURES "%s", cases[i].file);
        CHECK(expect(args, 1, cases[i].text));
    }
    return true;
}

/* El caso que justifica la herramienta: un CSV por proceso. Juntos
 * cuadran; el del productor solo parece una corrida con dos perdidas. */
static bool test_cli_crosses_several_files(void) {
    CHECK(expect(FIXTURES "multi_productor.csv " FIXTURES "multi_consumidor.csv",
                 0, "2 archivo(s), 4 filas de log"));
    CHECK(expect(FIXTURES "multi_productor.csv", 1, "mensaje perdido"));
    return true;
}

/* Una fila corrupta dice en que linea esta, en vez de descartar el
 * archivo sin explicacion. */
static bool test_cli_reports_bad_line(void) {
    CHECK(expect(FIXTURES "malformado.csv", 2, "linea 3"));
    return true;
}

static bool test_cli_missing_file(void) {
    CHECK(expect(FIXTURES "no_existe.csv", 2, "no_existe.csv"));
    return true;
}

static bool test_cli_usage(void) {
    CHECK(expect("", 2, "falta al menos un archivo"));
    CHECK(expect("--help", 0, "uso: verify"));
    return true;
}

int main(void) {
    struct test_case tests[] = {
        {"cli_clean_run", test_cli_clean_run},
        {"cli_each_violation", test_cli_each_violation},
        {"cli_crosses_several_files", test_cli_crosses_several_files},
        {"cli_reports_bad_line", test_cli_reports_bad_line},
        {"cli_missing_file", test_cli_missing_file},
        {"cli_usage", test_cli_usage},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
