/* creator.c — crea e inicializa una instancia con nombre y capacidad.
 *
 *   ./creator --name /soa_p2_g1 --capacity 8
 *
 * Retorna 0 solo cuando la instancia ya está publicada y acepta
 * participantes; una instancia existente se rechaza sin modificarla (D1).
 * El análisis de argumentos es mínimo y estricto; el Issue #4 lo unifica
 * con el resto de ejecutables.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"

static void usage(FILE *f) {
    fprintf(f, "uso: creator --name /<nombre> --capacity <1..%u>\n",
            INSTANCE_MAX_CAPACITY);
}

/* Entero decimal sin signo, completo y sin overflow. Rechaza vacío, signo,
 * espacios y basura al final. */
static bool parse_u32(const char *s, uint32_t *out) {
    if (s == NULL || *s < '0' || *s > '9')
        return false;
    errno = 0;
    char *end;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno == ERANGE || *end != '\0' || v > UINT32_MAX)
        return false;
    *out = (uint32_t)v;
    return true;
}

int main(int argc, char **argv) {
    const char *name = NULL;
    const char *cap_arg = NULL;

    for (int i = 1; i < argc; i++) {
        const char **slot = NULL;
        if (strcmp(argv[i], "--name") == 0)
            slot = &name;
        else if (strcmp(argv[i], "--capacity") == 0)
            slot = &cap_arg;
        else if (strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return EXIT_SUCCESS;
        } else {
            fprintf(stderr, "creator: opcion desconocida: %s\n", argv[i]);
            usage(stderr);
            return EXIT_FAILURE;
        }
        if (*slot != NULL) {
            fprintf(stderr, "creator: opcion repetida: %s\n", argv[i]);
            return EXIT_FAILURE;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "creator: falta el valor de %s\n", argv[i]);
            return EXIT_FAILURE;
        }
        *slot = argv[++i];
    }

    if (name == NULL || cap_arg == NULL) {
        fprintf(stderr, "creator: faltan argumentos obligatorios\n");
        usage(stderr);
        return EXIT_FAILURE;
    }

    uint32_t capacity;
    if (!parse_u32(cap_arg, &capacity)) {
        fprintf(stderr, "creator: capacidad no es un entero valido: '%s'\n", cap_arg);
        return EXIT_FAILURE;
    }

    struct instance *inst;
    size_t len;
    enum memory_result r = memory_create(name, capacity, &inst, &len);
    if (r != MEM_OK) {
        if (r == MEM_SYS_ERROR)
            fprintf(stderr, "creator: %s: %s\n", memory_result_str(r), strerror(errno));
        else
            fprintf(stderr, "creator: %s: %s\n", name, memory_result_str(r));
        return EXIT_FAILURE;
    }

    /* La instancia queda en el sistema después de desmapear: el objeto de
     * memoria compartida persiste hasta que el finisher hace shm_unlink. */
    memory_unmap(inst, len);
    printf("creator: instancia %s lista (capacidad %" PRIu32 ")\n", name, capacity);
    return EXIT_SUCCESS;
}
