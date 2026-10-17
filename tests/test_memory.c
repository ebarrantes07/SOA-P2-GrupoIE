/* test_memory.c — pruebas del módulo de memoria compartida (D1, D13,
 * Issue #9). Cada prueba usa un nombre propio con el pid para no chocar con
 * otras corridas y lo elimina al terminar.
 */
#include "memory.h"
#include "test_util.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *name_for(const char *tag) {
    static char buf[INSTANCE_NAME_MAX + 1];
    snprintf(buf, sizeof(buf), "/t9_%d_%s", (int)getpid(), tag);
    memory_unlink(buf);           /* restos de una corrida anterior abortada */
    return buf;
}

static bool test_segment_size(void) {
    CHECK(instance_segment_size(1) == sizeof(struct instance) + sizeof(struct message));
    CHECK(instance_segment_size(1024) == sizeof(struct instance) + 1024 * sizeof(struct message));
    return true;
}

static bool test_name_validation(void) {
    CHECK(instance_name_is_valid("/soa_p2_g1"));
    CHECK(instance_name_is_valid("/a"));
    CHECK(instance_name_is_valid("/A-b_9"));
    CHECK(instance_name_is_valid("/123456789012345678901234567890"));   /* 31 */
    CHECK(!instance_name_is_valid(NULL));
    CHECK(!instance_name_is_valid(""));
    CHECK(!instance_name_is_valid("/"));
    CHECK(!instance_name_is_valid("soa"));
    CHECK(!instance_name_is_valid("/a/b"));
    CHECK(!instance_name_is_valid("/con espacio"));
    CHECK(!instance_name_is_valid("/1234567890123456789012345678901"));  /* 32 */
    return true;
}

static bool test_create_initializes(void) {
    const char *name = name_for("init");
    struct instance *inst;
    size_t len;
    CHECK(memory_create(name, 8, &inst, &len) == MEM_OK);
    CHECK(len == instance_segment_size(8));
    CHECK(atomic_load(&inst->magic) == INSTANCE_MAGIC);
    CHECK(inst->version == INSTANCE_VERSION);
    CHECK(inst->capacity == 8);
    CHECK(inst->state == STATE_OPEN);
    CHECK(inst->head == 0 && inst->tail == 0 && inst->count == 0);
    CHECK(inst->enqueue_seq == 0 && inst->dequeue_seq == 0);
    CHECK(atomic_load(&inst->refs) == 0);
    CHECK(memory_unmap(inst, len) == 0);

    /* Otro mapeo ve la instancia publicada. */
    CHECK(memory_open(name, &inst, &len) == MEM_OK);
    CHECK(inst->capacity == 8);
    CHECK(memory_unmap(inst, len) == 0);
    CHECK(memory_unlink(name) == 0);
    return true;
}

static bool test_duplicate_leaves_first_untouched(void) {
    const char *name = name_for("dup");
    struct instance *first, *again;
    size_t len, len2;
    CHECK(memory_create(name, 4, &first, &len) == MEM_OK);
    first->enqueue_seq = 42;      /* marca para detectar si se reinicializa */

    CHECK(memory_create(name, 16, &again, &len2) == MEM_EXISTS);

    CHECK(memory_open(name, &again, &len2) == MEM_OK);
    CHECK(again->capacity == 4);
    CHECK(again->enqueue_seq == 42);
    memory_unmap(again, len2);
    memory_unmap(first, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

static bool test_invalid_args_create_nothing(void) {
    const char *name = name_for("bad");
    struct instance *inst;
    size_t len;
    CHECK(memory_create(name, 0, &inst, &len) == MEM_INVALID_CAPACITY);
    CHECK(memory_create(name, 1025, &inst, &len) == MEM_INVALID_CAPACITY);
    CHECK(memory_open(name, &inst, &len) == MEM_NOT_FOUND);
    CHECK(memory_create("sin_barra", 8, &inst, &len) == MEM_INVALID_NAME);
    return true;
}

static bool test_open_missing(void) {
    struct instance *inst;
    size_t len;
    CHECK(memory_open(name_for("none"), &inst, &len) == MEM_NOT_FOUND);
    return true;
}

/* Crea a mano un segmento del tamaño correcto y deja que la prueba lo
 * altere, para simular un creator a medias o un segmento ajeno. */
static struct instance *raw_segment(const char *name, size_t len) {
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1 || ftruncate(fd, (off_t)len) == -1)
        return NULL;
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return p == MAP_FAILED ? NULL : p;
}

static bool test_open_validates_header(void) {
    struct instance *inst, *raw;
    size_t len = instance_segment_size(8);

    /* magic en 0: creator que no terminó. */
    const char *name = name_for("unready");
    CHECK((raw = raw_segment(name, len)) != NULL);
    CHECK(memory_open(name, &inst, &len) == MEM_NOT_READY);

    /* Versión distinta. */
    raw->capacity = 8;
    raw->version = INSTANCE_VERSION + 1;
    atomic_store(&raw->magic, INSTANCE_MAGIC);
    CHECK(memory_open(name, &inst, &len) == MEM_INCOMPATIBLE);

    /* Capacidad que no corresponde al tamaño del segmento. 1024 y no 16:
     * en macOS el tamaño se redondea a página y 8 y 16 caben en la misma. */
    raw->version = INSTANCE_VERSION;
    raw->capacity = 1024;
    CHECK(memory_open(name, &inst, &len) == MEM_INCOMPATIBLE);

    raw->capacity = 8;
    CHECK(memory_open(name, &inst, &len) == MEM_OK);
    memory_unmap(inst, len);
    munmap(raw, instance_segment_size(8));
    CHECK(memory_unlink(name) == 0);

    /* Segmento recién creado, más chico que la cabecera (sin ftruncate). */
    name = name_for("tiny");
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd != -1);
    close(fd);
    CHECK(memory_open(name, &inst, &len) == MEM_NOT_READY);
    CHECK(memory_unlink(name) == 0);
    return true;
}

static bool test_recreate_after_unlink(void) {
    const char *name = name_for("again");
    struct instance *inst;
    size_t len;
    CHECK(memory_create(name, 2, &inst, &len) == MEM_OK);
    inst->enqueue_seq = 7;
    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);

    CHECK(memory_create(name, 3, &inst, &len) == MEM_OK);
    CHECK(inst->capacity == 3);
    CHECK(inst->enqueue_seq == 0);   /* instancia nueva, sin restos de la anterior */
    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

/* El mutex del segmento coordina procesos distintos: varios hijos
 * incrementan un contador compartido bajo el mutex sin perder cuentas. */
static bool test_mutex_shared_across_processes(void) {
    enum { CHILDREN = 4, ROUNDS = 20000 };
    const char *name = name_for("fork");
    struct instance *inst;
    size_t len;
    CHECK(memory_create(name, 1, &inst, &len) == MEM_OK);
    memory_unmap(inst, len);

    for (int c = 0; c < CHILDREN; c++) {
        pid_t pid = fork();
        CHECK(pid != -1);
        if (pid == 0) {
            struct instance *mine;
            size_t mlen;
            if (memory_open(name, &mine, &mlen) != MEM_OK)
                _exit(2);
            for (int i = 0; i < ROUNDS; i++) {
                if (pthread_mutex_lock(&mine->mutex) != 0)
                    _exit(3);
                mine->enqueue_seq++;
                pthread_mutex_unlock(&mine->mutex);
            }
            memory_unmap(mine, mlen);
            _exit(0);
        }
    }

    bool children_ok = true;
    for (int c = 0; c < CHILDREN; c++) {
        int status;
        if (wait(&status) == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            children_ok = false;
    }
    CHECK(children_ok);

    CHECK(memory_open(name, &inst, &len) == MEM_OK);
    CHECK(inst->enqueue_seq == (uint64_t)CHILDREN * ROUNDS);
    memory_unmap(inst, len);
    CHECK(memory_unlink(name) == 0);
    return true;
}

int main(void) {
    struct test_case tests[] = {
        {"segment_size", test_segment_size},
        {"name_validation", test_name_validation},
        {"create_initializes", test_create_initializes},
        {"duplicate_leaves_first_untouched", test_duplicate_leaves_first_untouched},
        {"invalid_args_create_nothing", test_invalid_args_create_nothing},
        {"open_missing", test_open_missing},
        {"open_validates_header", test_open_validates_header},
        {"recreate_after_unlink", test_recreate_after_unlink},
        {"mutex_shared_across_processes", test_mutex_shared_across_processes},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
