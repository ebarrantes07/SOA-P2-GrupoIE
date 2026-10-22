/* cli.c — análisis estricto de argumentos (D15).
 *
 * Una tabla dice qué opciones acepta cada rol y cuáles son obligatorias.
 * Cada valor se convierte en cuanto se lee, así el primer error es el que
 * se reporta.
 */
#include "cli.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"

enum opt_id {
    OPT_NAME, OPT_CAPACITY, OPT_ID, OPT_MEAN_MS, OPT_SEED,
    OPT_MESSAGES, OPT_LOG, OPT_SUMMARY, OPT_COUNT
};

#define ROLE(r)  (1u << (r))
#define WORKERS  (ROLE(CLI_PRODUCER) | ROLE(CLI_CONSUMER))
#define ALL      (ROLE(CLI_CREATOR) | WORKERS | ROLE(CLI_FINISHER))

static const struct {
    const char *flag;
    unsigned    allowed;    /* roles que aceptan la opción */
    unsigned    required;   /* roles para los que es obligatoria */
} SPECS[OPT_COUNT] = {
    [OPT_NAME]     = { "--name",     ALL,                   ALL },
    [OPT_CAPACITY] = { "--capacity", ROLE(CLI_CREATOR),     ROLE(CLI_CREATOR) },
    [OPT_ID]       = { "--id",       WORKERS,               WORKERS },
    [OPT_MEAN_MS]  = { "--mean-ms",  WORKERS,               WORKERS },
    [OPT_SEED]     = { "--seed",     WORKERS,               WORKERS },
    [OPT_MESSAGES] = { "--messages", ROLE(CLI_PRODUCER),    0 },
    [OPT_LOG]      = { "--log",      WORKERS | ROLE(CLI_FINISHER), WORKERS },
    [OPT_SUMMARY]  = { "--summary",  ROLE(CLI_FINISHER),    ROLE(CLI_FINISHER) },
};

static const char *const ROLE_NAMES[] = {
    [CLI_CREATOR] = "creator", [CLI_PRODUCER] = "producer",
    [CLI_CONSUMER] = "consumer", [CLI_FINISHER] = "finisher",
};

static const char *const USAGES[] = {
    [CLI_CREATOR]  = "--name /<nombre> --capacity <1..1024>",
    [CLI_PRODUCER] = "--name /<nombre> --id <n> --mean-ms <0..1000> --seed <n> "
                     "[--messages <n>] --log <archivo.csv>",
    [CLI_CONSUMER] = "--name /<nombre> --id <n> --mean-ms <0..1000> --seed <n> "
                     "--log <archivo.csv>",
    [CLI_FINISHER] = "--name /<nombre> --summary <archivo.csv> [--log <archivo.csv>]",
};

const char *cli_role_name(enum cli_role role) {
    return ROLE_NAMES[role];
}

void cli_usage(enum cli_role role, FILE *f) {
    fprintf(f, "uso: %s %s\n", ROLE_NAMES[role], USAGES[role]);
}

static enum cli_result fail(char *err, size_t err_len, const char *fmt, ...) {
    if (err != NULL && err_len > 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, err_len, fmt, ap);
        va_end(ap);
    }
    return CLI_ERROR;
}

/* ---- Conversores ------------------------------------------------------ */

static bool all_digits(const char *s) {
    if (s == NULL || *s == '\0')
        return false;
    for (; *s != '\0'; s++)
        if (*s < '0' || *s > '9')
            return false;
    return true;
}

bool cli_parse_u64(const char *s, uint64_t min, uint64_t max, uint64_t *out) {
    /* Solo dígitos: strtoull aceptaría espacios iniciales, '+' y '-' (que
     * convierte en un número enorme). */
    if (!all_digits(s))
        return false;
    errno = 0;
    char *end;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno == ERANGE || *end != '\0' || v < min || v > max)
        return false;
    *out = (uint64_t)v;
    return true;
}

bool cli_parse_u32(const char *s, uint32_t min, uint32_t max, uint32_t *out) {
    uint64_t v;
    if (!cli_parse_u64(s, min, max, &v))
        return false;
    *out = (uint32_t)v;
    return true;
}

bool cli_parse_mean_ms(const char *s, double *out) {
    /* Formato propio antes de strtod, que aceptaría "inf", "nan", "1e3",
     * "0x10", signo y espacios. */
    if (s == NULL || *s < '0' || *s > '9')
        return false;
    const char *p = s;
    while (*p >= '0' && *p <= '9')
        p++;
    if (*p == '.') {
        p++;
        if (*p < '0' || *p > '9')
            return false;           /* "5." sin decimales */
        while (*p >= '0' && *p <= '9')
            p++;
    }
    if (*p != '\0')
        return false;

    errno = 0;
    char *end;
    double v = strtod(s, &end);
    if (errno == ERANGE || *end != '\0' || !isfinite(v) || v < 0.0 || v > CLI_MEAN_MS_MAX)
        return false;
    *out = v;
    return true;
}

/* ---- Análisis --------------------------------------------------------- */

static enum cli_result convert(enum opt_id opt, const char *v,
                               struct cli_options *o, char *err, size_t err_len) {
    const char *flag = SPECS[opt].flag;
    switch (opt) {
    case OPT_NAME:
        if (!instance_name_is_valid(v))
            return fail(err, err_len, "%s invalido: '%s' (debe ser '/' seguido de 1 a 30 "
                        "caracteres [A-Za-z0-9_-])", flag, v);
        o->name = v;
        break;
    case OPT_CAPACITY:
        if (!cli_parse_u32(v, INSTANCE_MIN_CAPACITY, INSTANCE_MAX_CAPACITY, &o->capacity))
            return fail(err, err_len, "%s invalida: '%s' (entero de %u a %u)", flag, v,
                        INSTANCE_MIN_CAPACITY, INSTANCE_MAX_CAPACITY);
        break;
    case OPT_ID:
        if (!cli_parse_u32(v, 1, UINT32_MAX, &o->id))
            return fail(err, err_len, "%s invalido: '%s' (entero de 1 a %u)", flag, v,
                        (unsigned)UINT32_MAX);
        break;
    case OPT_MEAN_MS:
        if (!cli_parse_mean_ms(v, &o->mean_ms))
            return fail(err, err_len, "%s invalido: '%s' (numero de 0 a 1000, p. ej. 5 o 2.5)",
                        flag, v);
        break;
    case OPT_SEED:
        if (!cli_parse_u32(v, 1, UINT32_MAX, &o->seed))
            return fail(err, err_len, "%s invalida: '%s' (entero de 1 a %u; 0 no sirve "
                        "para xorshift32)", flag, v, (unsigned)UINT32_MAX);
        break;
    case OPT_MESSAGES:
        if (!cli_parse_u64(v, 1, UINT64_MAX, &o->messages))
            return fail(err, err_len, "%s invalido: '%s' (entero positivo)", flag, v);
        o->has_messages = true;
        break;
    case OPT_LOG:
    case OPT_SUMMARY:
        if (*v == '\0')
            return fail(err, err_len, "%s no puede ser una ruta vacia", flag);
        if (opt == OPT_LOG)
            o->log_path = v;
        else
            o->summary_path = v;
        break;
    case OPT_COUNT:
        break;
    }
    return CLI_OK;
}

enum cli_result cli_parse(enum cli_role role, int argc, char *const argv[],
                          struct cli_options *out, char *err, size_t err_len) {
    struct cli_options o = {0};
    bool seen[OPT_COUNT] = {false};
    unsigned bit = ROLE(role);

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--help") == 0)
            return CLI_HELP;

        int opt = -1;
        for (int k = 0; k < OPT_COUNT; k++)
            if (strcmp(arg, SPECS[k].flag) == 0)
                opt = k;

        if (opt < 0)
            return fail(err, err_len, "opcion desconocida: '%s'", arg);
        if (!(SPECS[opt].allowed & bit))
            return fail(err, err_len, "la opcion %s no aplica a este ejecutable", arg);
        if (seen[opt])
            return fail(err, err_len, "opcion repetida: %s", arg);
        /* Un valor que empieza con "--" es casi seguro la opción siguiente:
         * "--log --id 3" significa que falta el valor de --log. */
        if (i + 1 >= argc || strncmp(argv[i + 1], "--", 2) == 0)
            return fail(err, err_len, "falta el valor de %s", arg);

        seen[opt] = true;
        enum cli_result r = convert((enum opt_id)opt, argv[++i], &o, err, err_len);
        if (r != CLI_OK)
            return r;
    }

    for (int k = 0; k < OPT_COUNT; k++)
        if ((SPECS[k].required & bit) && !seen[k])
            return fail(err, err_len, "falta la opcion obligatoria %s", SPECS[k].flag);

    *out = o;
    return CLI_OK;
}
