/* test_buffer.c — pruebas de envoltura del buffer circular puro (D2,
 * Issue #10). Ver docs/decisiones_diseno.md.
 */
#include "buffer.h"
#include "memory.h"
#include "test_util.h"

#include <stdlib.h>

static struct instance *make_instance(uint32_t capacity) {
    struct instance *inst = calloc(1, sizeof(*inst) + (size_t)capacity * sizeof(struct message));
    inst->capacity = capacity;
    return inst;
}

static struct message msg_with(uint32_t producer_id, uint64_t sequence) {
    struct message m = {0};
    m.producer_id = producer_id;
    m.sequence = sequence;
    return m;
}

static bool test_initial_state(void) {
    struct instance *inst = make_instance(4);
    CHECK(buffer_is_empty(inst));
    CHECK(!buffer_is_full(inst));
    free(inst);
    return true;
}

static bool test_fill_drain_refill(void) {
    /* Capacidad 3: llenar fuerza a tail a envolver de 2 a 0 en el tercer
     * push. Extraer 1 no toca tail (solo head). Volver a insertar reusa
     * la casilla 0, recien liberada por el pop, antes de volver a
     * envolver. Al final, confirmar orden FIFO pese al envoltorio. */
    struct instance *inst = make_instance(3);

    struct message a = msg_with(1, 1), b = msg_with(1, 2), c = msg_with(1, 3);
    buffer_push(inst, &a);
    buffer_push(inst, &b);
    buffer_push(inst, &c);
    CHECK(buffer_is_full(inst));
    CHECK(inst->tail == 0);   /* envolvio: (2 + 1) % 3 */

    struct message out;
    buffer_pop(inst, &out);
    CHECK(out.sequence == 1);
    CHECK(!buffer_is_full(inst));
    CHECK(inst->head == 1);

    struct message d = msg_with(1, 4);
    buffer_push(inst, &d);   /* reusa la casilla 0; tail: 0 -> 1 */
    CHECK(buffer_is_full(inst));
    CHECK(inst->tail == 1);

    uint64_t expected[] = {2, 3, 4};
    for (size_t i = 0; i < 3; i++) {
        buffer_pop(inst, &out);
        CHECK(out.sequence == expected[i]);
    }
    CHECK(buffer_is_empty(inst));

    free(inst);
    return true;
}

static bool test_capacity_one(void) {
    struct instance *inst = make_instance(1);
    CHECK(!buffer_is_full(inst));

    struct message a = msg_with(9, 100);
    buffer_push(inst, &a);
    CHECK(buffer_is_full(inst));
    CHECK(!buffer_is_empty(inst));

    struct message out;
    buffer_pop(inst, &out);
    CHECK(out.sequence == 100);
    CHECK(buffer_is_empty(inst));

    struct message b = msg_with(9, 101);
    buffer_push(inst, &b);
    CHECK(buffer_is_full(inst));

    free(inst);
    return true;
}

int main(void) {
    struct test_case tests[] = {
        {"initial_state", test_initial_state},
        {"fill_drain_refill", test_fill_drain_refill},
        {"capacity_one", test_capacity_one},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
