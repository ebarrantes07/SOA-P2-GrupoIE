/* test_payload.c — pruebas del payload verificable (D1, D22, Issue #44).
 *
 * El valor del checksum esta CONGELADO en vectores fijos a proposito: se
 * calcularon con una implementacion independiente de FNV-1a escrita desde
 * la especificacion de D1, no con este codigo. Si alguien cambia el
 * algoritmo, el orden de los campos o el relleno, estas pruebas fallan en
 * vez de que la corrida del experimento salga con todos los payload_ok en
 * 0 sin explicacion.
 */
#include "payload.h"
#include "test_util.h"

#include <string.h>

/* El vector congelado depende del orden de bytes del host, porque D1
 * decide no normalizar endianness (la ejecucion es de un solo host). En un
 * host big-endian el checksum seria otro: que falle ESTA linea, con su
 * nombre, y no un valor hexadecimal sin contexto. */
static bool host_is_little_endian(void) {
    uint32_t x = 1;
    uint8_t first;
    memcpy(&first, &x, 1);
    return first == 1;
}

struct vector {
    uint32_t producer_id;
    uint64_t sequence;
    uint32_t checksum;
};

/* Calculados con scripts/fnv_ref.py (FNV-1a escrito desde D1, no desde
 * src/payload.c). Ese script reproduce cualquiera de estos valores. */
static const struct vector VECTORS[] = {
    {1,          1,                    0xdf8b4fb5u},
    {0,          0,                    0x5562d305u},   /* identidad en cero */
    {1,          2,                    0x84102cb6u},
    {2,          1,                    0x03d40156u},   /* mismo fill que el anterior */
    {7,          42,                   0x188a5578u},
    {0xFFFFFFFFu, 0xFFFFFFFFFFFFFFFFu, 0x56418109u},   /* desborde de la suma */
};
#define NVECTORS (sizeof(VECTORS) / sizeof(VECTORS[0]))

static bool test_frozen_vectors(void) {
    CHECK(host_is_little_endian());
    for (size_t i = 0; i < NVECTORS; i++) {
        uint8_t fill[PAYLOAD_FILL_SIZE];
        payload_fill(fill, VECTORS[i].producer_id, VECTORS[i].sequence);
        uint32_t got = payload_checksum(VECTORS[i].producer_id, VECTORS[i].sequence, fill);
        if (got != VECTORS[i].checksum) {
            fprintf(stderr, "  vector %zu (producer_id=%u): esperado 0x%08x, obtenido 0x%08x\n",
                    i, VECTORS[i].producer_id, VECTORS[i].checksum, got);
            return false;
        }
    }
    return true;
}

/* (1,2) y (2,1) dan el mismo fill — la formula usa la suma — pero el
 * checksum tiene que distinguirlos: entran como campos separados y en
 * orden fijo. Un hash que sumara los campos sin orden pasaria el resto de
 * las pruebas y fallaria esta. */
static bool test_field_order_matters(void) {
    uint8_t a[PAYLOAD_FILL_SIZE], b[PAYLOAD_FILL_SIZE];
    payload_fill(a, 1, 2);
    payload_fill(b, 2, 1);
    CHECK(memcmp(a, b, PAYLOAD_FILL_SIZE) == 0);
    CHECK(payload_checksum(1, 2, a) != payload_checksum(2, 1, b));
    return true;
}

static bool test_fill_formula(void) {
    uint8_t fill[PAYLOAD_FILL_SIZE];

    payload_fill(fill, 1, 1);
    for (unsigned i = 0; i < PAYLOAD_FILL_SIZE; i++)
        CHECK(fill[i] == (uint8_t)(2 + i));

    /* Acarreo del % 256: con 250 + 0 + i, el byte 6 vuelve a 0. */
    payload_fill(fill, 250, 0);
    CHECK(fill[5] == 255);
    CHECK(fill[6] == 0);
    CHECK(fill[7] == 1);

    /* Identidad maxima: el acarreo cruza el limite de 32 bits. Da el mismo
     * resultado que hacer la suma en 32 bits, porque el modulo solo mira el
     * byte bajo; el caso esta aca por el borde, no porque distinga las dos
     * aritmeticas. */
    payload_fill(fill, 0xFFFFFFFFu, 1);
    CHECK(fill[0] == 0);
    CHECK(fill[1] == 1);
    return true;
}

static bool test_build_sets_every_field(void) {
    struct payload p;
    memset(&p, 0xAA, sizeof p);          /* basura previa: build la sobrescribe toda */
    payload_build(&p, 7, 42);

    CHECK(p.producer_id == 7);
    CHECK(p.sequence == 42);
    uint8_t expected[PAYLOAD_FILL_SIZE];
    payload_fill(expected, 7, 42);
    CHECK(memcmp(p.fill, expected, PAYLOAD_FILL_SIZE) == 0);
    CHECK(p.checksum == 0x188a5578u);    /* mismo vector congelado de arriba */
    return true;
}

/* Un mensaje como lo deja el productor: identidad en el sobre y payload
 * armado para esa identidad. Los campos que asigna sync_put no los toca
 * payload_verify. */
static void make_message(struct message *msg, uint32_t producer_id, uint64_t sequence) {
    memset(msg, 0, sizeof *msg);
    msg->producer_id = producer_id;
    msg->sequence = sequence;
    msg->enqueue_seq = 99;
    msg->enqueue_time_ns = 123456789;
    payload_build(&msg->payload, producer_id, sequence);
}

static bool test_round_trip(void) {
    struct message msg;
    for (size_t i = 0; i < NVECTORS; i++) {
        make_message(&msg, VECTORS[i].producer_id, VECTORS[i].sequence);
        CHECK(msg.payload.checksum == VECTORS[i].checksum);
        CHECK(payload_verify(&msg));
    }
    return true;
}

/* El criterio del issue: mutar CADA byte de fill y confirmar que se nota
 * en los 16 casos. Dos mutaciones por byte, porque una sola podria caer
 * justo en el valor que ya tenia. */
static bool test_every_fill_byte_is_detected(void) {
    for (unsigned i = 0; i < PAYLOAD_FILL_SIZE; i++) {
        struct message msg;

        make_message(&msg, 3, 9);
        msg.payload.fill[i] ^= 0xFFu;
        if (payload_verify(&msg)) {
            fprintf(stderr, "  fill[%u] invertido no se detecto\n", i);
            return false;
        }

        make_message(&msg, 3, 9);
        msg.payload.fill[i] = (uint8_t)(msg.payload.fill[i] + 1u);
        if (payload_verify(&msg)) {
            fprintf(stderr, "  fill[%u] + 1 no se detecto\n", i);
            return false;
        }
    }
    return true;
}

/* Chequeo cruzado de D1: el sobre contra las copias de adentro. Esto es lo
 * que el verificador de #7 no puede ver desde el CSV. */
static bool test_identity_mismatch_is_detected(void) {
    struct message msg;

    make_message(&msg, 3, 9);
    msg.payload.producer_id = 4;
    CHECK(!payload_verify(&msg));

    make_message(&msg, 3, 9);
    msg.payload.sequence = 10;
    CHECK(!payload_verify(&msg));

    /* Al reves: el sobre cambiado y el payload intacto. Tambien falla,
     * porque el fill esperado ya no corresponde. */
    make_message(&msg, 3, 9);
    msg.producer_id = 4;
    CHECK(!payload_verify(&msg));

    make_message(&msg, 3, 9);
    msg.sequence = 10;
    CHECK(!payload_verify(&msg));
    return true;
}

/* El unico caso que distingue el memcmp del fill del chequeo de checksum:
 * contenido cambiado CON el checksum rehecho para que cuadre. La identidad
 * sigue coincidiendo en el sobre y en el payload, y el checksum guardado
 * es consistente con los bytes que viajan; lo que no cuadra es que esos
 * bytes no son los que corresponden a esa identidad. Sin esta prueba, el
 * memcmp se podria borrar y las demas seguirian pasando. */
static bool test_fill_from_another_identity_is_detected(void) {
    struct message msg;
    make_message(&msg, 3, 9);

    payload_fill(msg.payload.fill, 4, 10);          /* relleno de otra identidad */
    msg.payload.checksum = payload_checksum(3, 9, msg.payload.fill);   /* rehecho */

    CHECK(msg.payload.producer_id == msg.producer_id);
    CHECK(msg.payload.sequence == msg.sequence);
    CHECK(msg.payload.checksum == payload_checksum(3, 9, msg.payload.fill));
    CHECK(!payload_verify(&msg));
    return true;
}

static bool test_checksum_field_mismatch_is_detected(void) {
    struct message msg;

    make_message(&msg, 3, 9);
    msg.payload.checksum ^= 1u;          /* un solo bit del campo */
    CHECK(!payload_verify(&msg));

    make_message(&msg, 3, 9);
    msg.payload.checksum = 0;            /* payload nunca armado */
    CHECK(!payload_verify(&msg));
    return true;
}

/* Un payload en cero (casilla del buffer que nadie lleno) no debe pasar
 * por bueno salvo que la identidad tambien sea cero. */
static bool test_zeroed_payload_is_rejected(void) {
    struct message msg;
    memset(&msg, 0, sizeof msg);
    msg.producer_id = 1;
    msg.sequence = 1;
    CHECK(!payload_verify(&msg));

    /* El caso frontera honesto: identidad (0,0) con su payload bien
     * armado si pasa, y el checksum no es 0. */
    make_message(&msg, 0, 0);
    CHECK(msg.payload.checksum == 0x5562d305u);
    CHECK(payload_verify(&msg));
    return true;
}

int main(void) {
    struct test_case tests[] = {
        {"frozen_vectors", test_frozen_vectors},
        {"field_order_matters", test_field_order_matters},
        {"fill_formula", test_fill_formula},
        {"build_sets_every_field", test_build_sets_every_field},
        {"round_trip", test_round_trip},
        {"every_fill_byte_is_detected", test_every_fill_byte_is_detected},
        {"identity_mismatch_is_detected", test_identity_mismatch_is_detected},
        {"fill_from_another_identity_is_detected", test_fill_from_another_identity_is_detected},
        {"checksum_field_mismatch_is_detected", test_checksum_field_mismatch_is_detected},
        {"zeroed_payload_is_rejected", test_zeroed_payload_is_rejected},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
