/* e2e_util.h — utilidades de las pruebas de punta a punta (#19, #22):
 * lanzan los ejecutables reales (./creator, ./producer, ...) como procesos
 * hijos, con tiempo límite, y miran su código de salida y su stderr.
 *
 * Todo es static: cada tests/test_*.c es su propio binario y no hay un
 * módulo de pruebas compartido que enlazar (Makefile).
 */
#ifndef SOA_P2_E2E_UTIL_H
#define SOA_P2_E2E_UTIL_H

#include "memory.h"

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define E2E_ARGV_MAX 24

static inline void e2e_sleep_ms(long ms) {
    struct timespec d = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&d, NULL);
}

/* ---- Nombres y archivos con limpieza ---------------------------------- */

/* Todo se borra con atexit: CHECK sale de la prueba en el acto y se
 * saltearía la limpieza, dejando segmentos en /dev/shm. */
static char e2e_names[32][INSTANCE_NAME_MAX + 1];
static int e2e_n_names;
static char e2e_files[128][96];
static int e2e_n_files;

static inline void e2e_cleanup(void) {
    for (int i = 0; i < e2e_n_names; i++)
        memory_unlink(e2e_names[i]);
    for (int i = 0; i < e2e_n_files; i++)
        unlink(e2e_files[i]);
}

/* Nombre de instancia único por proceso de prueba y etiqueta; borra uno
 * viejo con el mismo nombre si una corrida anterior murió a medias. */
static inline const char *e2e_name(const char *prefix, const char *tag) {
    if (e2e_n_names == (int)(sizeof(e2e_names) / sizeof(e2e_names[0])))
        abort();
    char *b = e2e_names[e2e_n_names++];
    snprintf(b, sizeof(e2e_names[0]), "/%s_%d_%s", prefix, (int)getpid(), tag);
    memory_unlink(b);
    return b;
}

/* Ruta en build/ (make test ya la creó), borrada al salir. */
static inline const char *e2e_path(const char *prefix, const char *tag, const char *who) {
    if (e2e_n_files == (int)(sizeof(e2e_files) / sizeof(e2e_files[0])))
        abort();
    char *b = e2e_files[e2e_n_files++];
    snprintf(b, sizeof(e2e_files[0]), "build/%s_%d_%s_%s", prefix, (int)getpid(), tag, who);
    unlink(b);
    return b;
}

/* ---- Procesos --------------------------------------------------------- */

/* Lanza exe con los argumentos dados (lista terminada en NULL). stdout va
 * a /dev/null; stderr a err_path, o a /dev/null si es NULL. */
static inline pid_t e2e_spawnv(const char *err_path, const char *exe, va_list ap) {
    const char *argv[E2E_ARGV_MAX];
    int n = 0;
    argv[n++] = exe;
    const char *a;
    while ((a = va_arg(ap, const char *)) != NULL) {
        if (n == E2E_ARGV_MAX - 1)
            abort();
        argv[n++] = a;
    }
    argv[n] = NULL;

    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        int out = open("/dev/null", O_WRONLY);
        int err = err_path != NULL ? open(err_path, O_WRONLY | O_CREAT | O_TRUNC, 0644) : out;
        if (out != -1)
            dup2(out, STDOUT_FILENO);
        if (err != -1)
            dup2(err, STDERR_FILENO);
        execv(exe, (char *const *)argv);
        _exit(127);
    }
    return pid;
}

static inline pid_t e2e_spawn(const char *err_path, const char *exe, ...) {
    va_list ap;
    va_start(ap, exe);
    pid_t pid = e2e_spawnv(err_path, exe, ap);
    va_end(ap);
    return pid;
}

/* Espera a pid hasta `seconds`. Devuelve el código de salida, o -1 si
 * murió por señal o no terminó a tiempo (y entonces lo mata): una prueba
 * que no progresa falla en vez de colgar make test. */
static inline int e2e_wait(pid_t pid, int seconds) {
    if (pid <= 0)
        return -1;
    for (int i = 0; i < seconds * 100; i++) {
        int st;
        if (waitpid(pid, &st, WNOHANG) == pid)
            return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        e2e_sleep_ms(10);
    }
    fprintf(stderr, "  el proceso %ld no termino en %d s\n", (long)pid, seconds);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

/* Corre exe hasta que termine (máximo 10 s) y devuelve su código. */
static inline int e2e_run(const char *err_path, const char *exe, ...) {
    va_list ap;
    va_start(ap, exe);
    pid_t pid = e2e_spawnv(err_path, exe, ap);
    va_end(ap);
    return e2e_wait(pid, 10);
}

/* ---- Archivos e instancias -------------------------------------------- */

/* true si el archivo existe y contiene `needle`. Si no, muestra lo que
 * tenía, para que la falla se entienda sin volver a correr nada. */
static inline bool e2e_file_has(const char *path, const char *needle) {
    char buf[4096];
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "  no se pudo leer %s\n", path);
        return false;
    }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (strstr(buf, needle) != NULL)
        return true;
    fprintf(stderr, "  se esperaba \"%s\" en stderr y llego: %s\n", needle, buf);
    return false;
}

static inline bool e2e_file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

/* true si el nombre de instancia existe y está publicada. */
static inline bool e2e_instance_exists(const char *name) {
    struct instance *inst;
    size_t len;
    if (memory_open(name, &inst, &len) != MEM_OK)
        return false;
    memory_unmap(inst, len);
    return true;
}

#endif /* SOA_P2_E2E_UTIL_H */
