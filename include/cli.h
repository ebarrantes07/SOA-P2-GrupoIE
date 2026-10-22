/* cli.h — análisis y validación estricta de los argumentos de los cuatro
 * ejecutables (interfaz mínima del enunciado).
 *
 * Decisión documentada en docs/decisiones_diseno.md (D15). El módulo solo
 * convierte y valida texto: no abre archivos ni toca la instancia, así que
 * un rechazo nunca deja recursos a medias.
 */
#ifndef SOA_P2_CLI_H
#define SOA_P2_CLI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum cli_role { CLI_CREATOR = 0, CLI_PRODUCER, CLI_CONSUMER, CLI_FINISHER };

#define CLI_MEAN_MS_MAX  1000.0

/* Valores ya validados. Los punteros apuntan a argv (no se copian). Un
 * campo que el rol no usa queda en 0 / NULL. */
struct cli_options {
    const char *name;           /* todos */
    uint32_t    capacity;       /* creator: 1..1024 */
    uint32_t    id;             /* producer, consumer: 1..UINT32_MAX */
    double      mean_ms;        /* producer, consumer: [0, 1000]; 0 desactiva las pausas */
    uint32_t    seed;           /* producer, consumer: 1..UINT32_MAX (0 atasca xorshift32) */
    bool        has_messages;   /* producer: false = sin cuota, sigue hasta el cierre */
    uint64_t    messages;       /* producer: cuota positiva si has_messages */
    const char *log_path;       /* producer, consumer (obligatorio); finisher (opcional, D12) */
    const char *summary_path;   /* finisher */
};

enum cli_result {
    CLI_OK = 0,
    CLI_HELP,                   /* se pidió --help: imprimir uso y salir con 0 */
    CLI_ERROR                   /* err contiene el motivo */
};

/* Analiza argv[1..argc-1] para el rol dado. En CLI_ERROR escribe en err
 * (si no es NULL) un mensaje en español sin salto de línea final y deja
 * *out sin garantías. */
enum cli_result cli_parse(enum cli_role role, int argc, char *const argv[],
                          struct cli_options *out, char *err, size_t err_len);

/* Línea de uso del rol, terminada en salto de línea. */
void cli_usage(enum cli_role role, FILE *f);

/* Nombre del ejecutable de un rol ("creator", "producer", ...). */
const char *cli_role_name(enum cli_role role);

/* ---- Conversores estrictos (expuestos para las pruebas) --------------- */

/* Entero decimal sin signo dentro de [min, max]. Rechaza vacío, signo,
 * espacios, basura al final y overflow. */
bool cli_parse_u32(const char *s, uint32_t min, uint32_t max, uint32_t *out);
bool cli_parse_u64(const char *s, uint64_t min, uint64_t max, uint64_t *out);

/* Media en milisegundos: dígitos con parte decimal opcional ("5", "2.5"),
 * sin signo, exponente, "inf" ni "nan", dentro de [0, 1000]. */
bool cli_parse_mean_ms(const char *s, double *out);

#endif /* SOA_P2_CLI_H */
