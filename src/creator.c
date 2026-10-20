/* creator.c — crea e inicializa una instancia con nombre y capacidad.
 *
 *   ./creator --name /soa_p2_g1 --capacity 8
 *
 * Retorna 0 solo cuando la instancia ya está publicada y acepta
 * participantes; una instancia existente se rechaza sin modificarla (D1).
 * Los argumentos se validan con el módulo común cli (D15).
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "memory.h"

int main(int argc, char **argv) {
    struct cli_options opt;
    char err[256];

    switch (cli_parse(CLI_CREATOR, argc, argv, &opt, err, sizeof(err))) {
    case CLI_HELP:
        cli_usage(CLI_CREATOR, stdout);
        return EXIT_SUCCESS;
    case CLI_ERROR:
        fprintf(stderr, "creator: %s\n", err);
        cli_usage(CLI_CREATOR, stderr);
        return EXIT_FAILURE;
    case CLI_OK:
        break;
    }

    struct instance *inst;
    size_t len;
    enum memory_result r = memory_create(opt.name, opt.capacity, &inst, &len);
    if (r != MEM_OK) {
        if (r == MEM_SYS_ERROR)
            fprintf(stderr, "creator: %s: %s\n", memory_result_str(r), strerror(errno));
        else
            fprintf(stderr, "creator: %s: %s\n", opt.name, memory_result_str(r));
        return EXIT_FAILURE;
    }

    /* La instancia queda en el sistema después de desmapear: el objeto de
     * memoria compartida persiste hasta que el finisher hace shm_unlink. */
    memory_unmap(inst, len);
    printf("creator: instancia %s lista (capacidad %" PRIu32 ")\n", opt.name, opt.capacity);
    return EXIT_SUCCESS;
}
