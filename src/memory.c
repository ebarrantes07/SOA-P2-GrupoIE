/* memory.c — creación, apertura y eliminación del segmento compartido.
 *
 * Implementa la secuencia de creación y la tabla de deshacer de D1, más las
 * validaciones al abrir de D13. No toca el contador de conexiones (D9) ni
 * el registro: eso vive en participants.c.
 */
#include "memory.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sync.h"

size_t instance_segment_size(uint32_t capacity) {
    return sizeof(struct instance) + (size_t)capacity * sizeof(struct message);
}

bool instance_name_is_valid(const char *name) {
    if (name == NULL || name[0] != '/')
        return false;
    size_t len = strlen(name);
    if (len < 2 || len > INSTANCE_NAME_MAX)
        return false;
    for (size_t i = 1; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

const char *memory_result_str(enum memory_result r) {
    switch (r) {
    case MEM_OK:               return "ok";
    case MEM_INVALID_NAME:     return "nombre invalido: debe ser '/' seguido de 1 a 30 caracteres [A-Za-z0-9_-]";
    case MEM_INVALID_CAPACITY: return "capacidad fuera de rango (1..1024)";
    case MEM_EXISTS:           return "ya existe una instancia con ese nombre";
    case MEM_NOT_FOUND:        return "no existe una instancia con ese nombre";
    case MEM_NOT_READY:        return "la instancia no esta lista (creator sin terminar o segmento ajeno)";
    case MEM_INCOMPATIBLE:     return "la instancia no es compatible con este binario (version, capacidad o tamano)";
    case MEM_SYS_ERROR:        return "error del sistema";
    }
    return "resultado desconocido";
}

/* Linux reporta en st_size el tamaño exacto pedido con ftruncate; macOS lo
 * redondea al tamaño de página. Se acepta el exacto o ese redondeo, nada
 * más (D13). */
static bool segment_size_matches(size_t expected, size_t actual) {
    long page = sysconf(_SC_PAGESIZE);
    size_t rounded = expected;
    if (page > 0)
        rounded = (expected + (size_t)page - 1) / (size_t)page * (size_t)page;
    return actual == expected || actual == rounded;
}

/* ftruncate puede interrumpirse por una señal; se reintenta (D13). */
static int ftruncate_retry(int fd, off_t len) {
    int rc;
    do {
        rc = ftruncate(fd, len);
    } while (rc == -1 && errno == EINTR);
    return rc;
}

enum memory_result memory_create(const char *name, uint32_t capacity,
                                 struct instance **out, size_t *map_len) {
    /* Paso 1: validar. Todavía no se creó nada. */
    if (!instance_name_is_valid(name))
        return MEM_INVALID_NAME;
    if (capacity < INSTANCE_MIN_CAPACITY || capacity > INSTANCE_MAX_CAPACITY)
        return MEM_INVALID_CAPACITY;

    /* Paso 2: creación exclusiva. Con EEXIST no se hace shm_unlink: ese
     * segmento es de otra instancia viva. */
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1)
        return errno == EEXIST ? MEM_EXISTS : MEM_SYS_ERROR;

    size_t len = instance_segment_size(capacity);
    int saved;

    /* Paso 3. */
    if (ftruncate_retry(fd, (off_t)len) == -1)
        goto undo_fd;

    /* Paso 4. */
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
        goto undo_fd;

    /* Paso 5: el mapeo sobrevive sin el descriptor. */
    close(fd);

    struct instance *inst = p;

    /* Paso 6: deshace por sí misma las primitivas que alcanzó a crear. */
    int rc = sync_init_primitives(inst);
    if (rc != 0) {
        munmap(p, len);
        shm_unlink(name);
        errno = -rc;
        return MEM_SYS_ERROR;
    }

    /* Paso 7. El resto de campos ya vale 0: el kernel entrega el segmento
     * en ceros. */
    inst->version = INSTANCE_VERSION;
    inst->capacity = capacity;
    inst->state = STATE_OPEN;

    /* Paso 8, último: publicar. */
    atomic_store_explicit(&inst->magic, INSTANCE_MAGIC, memory_order_release);

    *out = inst;
    *map_len = len;
    return MEM_OK;

undo_fd:
    saved = errno;
    close(fd);
    shm_unlink(name);
    errno = saved;
    return MEM_SYS_ERROR;
}

enum memory_result memory_open(const char *name, struct instance **out,
                               size_t *map_len) {
    if (!instance_name_is_valid(name))
        return MEM_INVALID_NAME;

    int fd = shm_open(name, O_RDWR, 0);
    if (fd == -1)
        return errno == ENOENT ? MEM_NOT_FOUND : MEM_SYS_ERROR;

    struct stat st;
    if (fstat(fd, &st) == -1) {
        int saved = errno;
        close(fd);
        errno = saved;
        return MEM_SYS_ERROR;
    }

    /* Un segmento más chico que la cabecera es de un creator que no
     * terminó (aún sin ftruncate) o no es nuestro: no se puede ni leer magic. */
    if (st.st_size < (off_t)sizeof(struct instance)) {
        close(fd);
        return MEM_NOT_READY;
    }

    size_t len = (size_t)st.st_size;
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int saved = errno;
    close(fd);
    if (p == MAP_FAILED) {
        errno = saved;
        return MEM_SYS_ERROR;
    }

    struct instance *inst = p;
    enum memory_result r = MEM_OK;

    /* acquire: si vemos el magic, vemos todo lo que el creator escribió antes. */
    if (atomic_load_explicit(&inst->magic, memory_order_acquire) != INSTANCE_MAGIC)
        r = MEM_NOT_READY;
    else if (inst->version != INSTANCE_VERSION ||
             inst->capacity < INSTANCE_MIN_CAPACITY ||
             inst->capacity > INSTANCE_MAX_CAPACITY ||
             !segment_size_matches(instance_segment_size(inst->capacity), len))
        r = MEM_INCOMPATIBLE;

    if (r != MEM_OK) {
        munmap(p, len);
        return r;
    }

    *out = inst;
    *map_len = len;
    return MEM_OK;
}

int memory_unmap(struct instance *inst, size_t map_len) {
    return munmap(inst, map_len) == 0 ? 0 : -errno;
}

int memory_unlink(const char *name) {
    return shm_unlink(name) == 0 ? 0 : -errno;
}
