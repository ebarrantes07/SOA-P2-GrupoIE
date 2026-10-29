/* test_concurrency.c — casos "FIFO basico", "Concurrencia", "Capacidad
 * minima" y "Estres" del enunciado (Issue #20). Los cuatro son la misma
 * corrida con distintos numeros: N productores y M consumidores de verdad
 * (./producer y ./consumer, no simulados), capacidad fija, cuota finita por
 * productor, y al final ./verify sobre todos los CSV.
 *
 * Lo que se afirma en cada caso tiene dos fuentes independientes:
 *
 *   - los contadores de la instancia, leidos bajo el mutex: aceptados,
 *     extraidos y ocupancia final tienen que cuadrar con N * cuota;
 *   - ./verify (#7, D16) sobre los CSV: sin perdidas, duplicados, contenido
 *     alterado ni violaciones de FIFO, y un cierre consistente.
 *
 * El corte a CLOSING lo hace esta prueba con closing_begin, como en
 * test_blocking.c, y no ./finisher: asi la instancia sigue mapeada despues
 * del corte y se pueden leer los contadores bajo el mutex. El protocolo de
 * cierre completo con el ejecutable real es el Issue #22.
 *
 * Las pausas van en 0 a proposito (ver D30): maximizan la contencion sobre
 * el buffer, que es lo que hace aparecer una carrera. La variedad de medias
 * de pausa vive en el experimento (#24, D27).
 */
#include "clock.h"
#include "logger.h"
#include "memory.h"
#include "participants.h"
#include "sync.h"
#include "test_util.h"

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* El caso de estres: 32 productores y 32 consumidores, el minimo que pide
 * el enunciado y la mitad del limite de 64 casillas por rol (D8). */
#define MAX_SIDE        32
#define LOG_PATH_MAX    64

/* Margen de espera por proceso. Una corrida sana de 4000 mensajes toma
 * ~0.3 s; 30 s es el plazo tras el cual se declara deadlock en vez de
 * colgar la suite. */
#define WAIT_SECONDS    30

struct run {
    const char       *tag;
    const char       *name;
    struct instance  *inst;
    size_t            map_len;
    unsigned          n_prod, n_cons;
    unsigned long     quota;           /* mensajes por productor */
    pid_t             prod[MAX_SIDE], cons[MAX_SIDE];
    char              prod_log[MAX_SIDE][LOG_PATH_MAX];
    char              cons_log[MAX_SIDE][LOG_PATH_MAX];
    char              fin_log[LOG_PATH_MAX];
    struct csv_logger fin;
};

/* ---- Utilidades ------------------------------------------------------- */

static void sleep_ms(long ms) {
    struct timespec d = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&d, NULL);
}

/* Los nombres se borran con atexit: CHECK sale en el acto y se saltearia la
 * limpieza, dejando el segmento en /dev/shm (igual que en #14, #15 y #21). */
static char all_names[8][INSTANCE_NAME_MAX + 1];
static volatile sig_atomic_t n_names;

static void unlink_all_names(void) {
    for (int i = 0; i < n_names; i++)
        memory_unlink(all_names[i]);
}

static const char *name_for(const char *tag) {
    if (n_names == (sig_atomic_t)(sizeof(all_names) / sizeof(all_names[0])))
        abort();
    char *b = all_names[n_names++];
    snprintf(b, sizeof(all_names[0]), "/t20_%d_%s", (int)getpid(), tag);
    memory_unlink(b);
    return b;
}

/* ---- Hijos vivos ------------------------------------------------------ */

/* Cada proceso lanzado queda anotado aca hasta que wait_exit lo cosecha, y
 * lo que quede se mata al salir. Importa en los caminos de falla: si
 * run_join se va antes del corte a CLOSING (porque un productor fallo), los
 * consumidores siguen esperando datos que ya nadie va a insertar, y sin
 * esto quedarian bloqueados para siempre -- 32 de ellos en el caso de
 * estres -- aunque la prueba ya haya terminado.
 *
 * kill() sobre un pid anotado no puede caer en un proceso ajeno: un hijo
 * que el padre todavia no cosecho sigue siendo zombie o vivo, asi que el
 * nucleo no puede reciclar su pid. */
static pid_t live[2 * MAX_SIDE];
static volatile sig_atomic_t n_live;

static void remember_child(pid_t pid) {
    if (n_live == (sig_atomic_t)(sizeof(live) / sizeof(live[0])))
        abort();
    live[n_live++] = pid;
}

static void forget_child(pid_t pid) {
    for (sig_atomic_t i = 0; i < n_live; i++)
        if (live[i] == pid) {
            live[i] = live[--n_live];
            return;
        }
}

static void kill_live_children(void) {
    for (sig_atomic_t i = 0; i < n_live; i++) {
        kill(live[i], SIGKILL);
        waitpid(live[i], NULL, 0);
    }
    n_live = 0;
}

static pid_t spawn(const char *exe, const char *name, unsigned id,
                   const char *messages, const char *log) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        char idbuf[16];
        snprintf(idbuf, sizeof(idbuf), "%u", id);
        if (messages != NULL)
            execl(exe, exe, "--name", name, "--id", idbuf, "--mean-ms", "0",
                  "--seed", "7", "--messages", messages, "--log", log, (char *)NULL);
        else
            execl(exe, exe, "--name", name, "--id", idbuf, "--mean-ms", "0",
                  "--seed", "7", "--log", log, (char *)NULL);
        _exit(127);
    }
    if (pid != -1)
        remember_child(pid);
    return pid;
}

/* Codigo de salida del proceso, o -1 si no termino en `seconds`. Un -1 es
 * el sintoma de deadlock: el proceso no avanza y nadie lo va a despertar. */
static int wait_exit(pid_t pid, int seconds) {
    for (int i = 0; i < seconds * 100; i++) {
        int st;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            forget_child(pid);
            return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        }
        sleep_ms(10);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    forget_child(pid);
    return -1;
}

/* Los contadores de la instancia, leidos en UNA region critica: afuera del
 * mutex dos lecturas sueltas quedan mutuamente inconsistentes, porque los
 * participantes siguen trabajando entre una y otra. */
struct counters {
    uint32_t count, active_producers, active_consumers;
    uint64_t enqueue_seq, dequeue_seq;
};

static struct counters read_counters(struct instance *inst) {
    sync_lock(inst);
    struct counters c = { inst->count, inst->active_producers,
                          inst->active_consumers, inst->enqueue_seq,
                          inst->dequeue_seq };
    sync_unlock(inst);
    return c;
}

/* Espera a que los `n` consumidores esten registrados. Los consumidores no
 * se retiran hasta el cierre, asi que una vez alcanzado el numero no baja:
 * no hay carrera en esperarlo. Con los productores no se puede hacer lo
 * mismo, porque uno con cuota corta puede terminar y retirarse antes de que
 * el ultimo se registre. */
static bool wait_consumers(struct instance *inst, unsigned n, int seconds) {
    for (int i = 0; i < seconds * 100; i++) {
        if (read_counters(inst).active_consumers == n)
            return true;
        sleep_ms(10);
    }
    return false;
}

/* Corte a CLOSING con su fila, para que los consumidores drenen y
 * terminen. Misma secuencia que el ejecutable finisher (D21): capturar el
 * snapshot bajo el mutex y escribir la fila despues.
 *
 * Esta repetido de test_blocking.c en vez de vivir en test_util.h a
 * proposito: test_util.h no incluye ningun header del proyecto (solo
 * stdbool/stddef/stdio), y moverlo ahi le metaria memory.h y logger.h a
 * TODAS las pruebas, incluidas las de modulos que no usan la instancia. */
static bool cut(struct instance *inst, struct csv_logger *f) {
    struct close_snapshot snap;
    if (closing_begin(inst, &snap) != CLOSE_OK)
        return false;
    struct csv_record r;
    csv_record_event(&r, CSV_CLOSING, snap.closing_ns, NULL);
    r.enqueue_seq = snap.accepted;
    r.dequeue_seq = snap.extracted;
    r.occupancy = snap.pending;
    return csv_logger_write(f, &r) == 0;
}

/* ---- Arnes de una corrida --------------------------------------------- */

/* Crea la instancia, abre el CSV del corte, levanta los consumidores,
 * espera a que esten todos registrados y recien entonces levanta los
 * productores. Ese orden evita que un productor inserte antes de que haya
 * quien drene, lo que dejaria el buffer lleno hasta que llegue el primer
 * consumidor; no cambia lo que se verifica, pero hace la corrida
 * reproducible. */
static bool run_start(struct run *r, const char *tag, uint32_t capacity,
                      unsigned n_prod, unsigned n_cons, unsigned long quota) {
    memset(r, 0, sizeof(*r));
    r->tag = tag;
    r->n_prod = n_prod;
    r->n_cons = n_cons;
    r->quota = quota;
    if (n_prod > MAX_SIDE || n_cons > MAX_SIDE)
        return false;

    r->name = name_for(tag);
    if (memory_create(r->name, capacity, &r->inst, &r->map_len) != MEM_OK) {
        fprintf(stderr, "  %s: no se pudo crear la instancia\n", tag);
        return false;
    }

    snprintf(r->fin_log, sizeof(r->fin_log), "build/t20_%d_%s_f.csv",
             (int)getpid(), tag);
    if (csv_logger_open(&r->fin, r->fin_log, CSV_FINISHER, 0) != 0) {
        fprintf(stderr, "  %s: no se pudo abrir %s\n", tag, r->fin_log);
        return false;
    }

    for (unsigned i = 0; i < n_cons; i++) {
        snprintf(r->cons_log[i], LOG_PATH_MAX, "build/t20_%d_%s_c%u.csv",
                 (int)getpid(), tag, i + 1);
        r->cons[i] = spawn("./consumer", r->name, i + 1, NULL, r->cons_log[i]);
    }
    if (!wait_consumers(r->inst, n_cons, WAIT_SECONDS)) {
        fprintf(stderr, "  %s: solo %u de %u consumidores se registraron\n", tag,
                read_counters(r->inst).active_consumers, n_cons);
        return false;
    }

    char quotabuf[24];
    snprintf(quotabuf, sizeof(quotabuf), "%lu", quota);
    for (unsigned i = 0; i < n_prod; i++) {
        snprintf(r->prod_log[i], LOG_PATH_MAX, "build/t20_%d_%s_p%u.csv",
                 (int)getpid(), tag, i + 1);
        r->prod[i] = spawn("./producer", r->name, i + 1, quotabuf, r->prod_log[i]);
    }
    return true;
}

/* Espera a los productores (que se retiran al cumplir su cuota), corta a
 * CLOSING, espera a los consumidores (que drenan lo aceptado) y comprueba
 * la conservacion contra los contadores de la instancia. */
static bool run_join(struct run *r) {
    uint64_t expected = (uint64_t)r->n_prod * r->quota;

    for (unsigned i = 0; i < r->n_prod; i++) {
        int code = wait_exit(r->prod[i], WAIT_SECONDS);
        if (code != 0) {
            fprintf(stderr, "  %s: productor %u termino con %d%s\n", r->tag, i + 1,
                    code, code == -1 ? " (no termino en el plazo: deadlock)" : "");
            return false;
        }
    }
    struct counters c = read_counters(r->inst);
    if (c.enqueue_seq != expected || c.active_producers != 0) {
        fprintf(stderr, "  %s: %llu aceptados y %u productores activos; "
                        "se esperaban %llu y 0\n", r->tag,
                (unsigned long long)c.enqueue_seq, c.active_producers,
                (unsigned long long)expected);
        return false;
    }

    if (!cut(r->inst, &r->fin)) {
        fprintf(stderr, "  %s: el corte a CLOSING fallo\n", r->tag);
        return false;
    }

    for (unsigned i = 0; i < r->n_cons; i++) {
        int code = wait_exit(r->cons[i], WAIT_SECONDS);
        if (code != 0) {
            fprintf(stderr, "  %s: consumidor %u termino con %d%s\n", r->tag, i + 1,
                    code, code == -1 ? " (no termino en el plazo: deadlock)" : "");
            return false;
        }
    }
    c = read_counters(r->inst);
    if (c.dequeue_seq != expected || c.count != 0 || c.active_consumers != 0) {
        fprintf(stderr, "  %s: %llu extraidos, %u en el buffer y %u consumidores "
                        "activos; se esperaban %llu, 0 y 0\n", r->tag,
                (unsigned long long)c.dequeue_seq, c.count, c.active_consumers,
                (unsigned long long)expected);
        return false;
    }
    return csv_logger_close(&r->fin) == 0;
}

/* Corre ./verify sobre los CSV de la corrida. El comando se arma en un
 * buffer del tamano justo porque el caso de estres pasa 65 archivos. La
 * salida se guarda y solo se imprime si hubo violaciones: verify las limita
 * a 20 (MAX_PRINTED), asi que el reporte entra holgado en el buffer, y lo
 * que sobre se descarta leyendo hasta EOF para no dejar a verify bloqueado
 * escribiendo en una tuberia llena. */
static int run_verify(const struct run *r) {
    size_t n = r->n_prod + r->n_cons + 1;
    size_t len = sizeof("./verify") + n * (LOG_PATH_MAX + 1);
    char *cmd = malloc(len);
    if (cmd == NULL)
        return -1;

    size_t at = (size_t)snprintf(cmd, len, "./verify");
    for (unsigned i = 0; i < r->n_prod; i++)
        at += (size_t)snprintf(cmd + at, len - at, " %s", r->prod_log[i]);
    for (unsigned i = 0; i < r->n_cons; i++)
        at += (size_t)snprintf(cmd + at, len - at, " %s", r->cons_log[i]);
    snprintf(cmd + at, len - at, " %s", r->fin_log);

    FILE *p = popen(cmd, "r");
    free(cmd);
    if (p == NULL)
        return -1;

    char out[8192];
    size_t have = 0;
    for (;;) {
        char chunk[1024];
        size_t got = fread(chunk, 1, sizeof(chunk), p);
        if (got == 0)
            break;
        size_t room = sizeof(out) - 1 - have;
        size_t take = got < room ? got : room;
        memcpy(out + have, chunk, take);
        have += take;
    }
    out[have] = '\0';

    int st = pclose(p);
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (code != 0)
        fprintf(stderr, "%s", out);
    return code;
}

/* Borra los CSV y el segmento. Solo se llama cuando el caso paso: si fallo,
 * los CSV quedan en build/ para poder mirarlos. */
static void run_cleanup(struct run *r) {
    for (unsigned i = 0; i < r->n_prod; i++)
        unlink(r->prod_log[i]);
    for (unsigned i = 0; i < r->n_cons; i++)
        unlink(r->cons_log[i]);
    unlink(r->fin_log);
    memory_unmap(r->inst, r->map_len);
    memory_unlink(r->name);
}

/* Una corrida completa: levantar, esperar, cortar, verificar y limpiar. */
static bool run_case(const char *tag, uint32_t capacity, unsigned n_prod,
                     unsigned n_cons, unsigned long quota) {
    struct run r;
    CHECK(run_start(&r, tag, capacity, n_prod, n_cons, quota));
    CHECK(run_join(&r));
    CHECK(run_verify(&r) == 0);
    run_cleanup(&r);
    return true;
}

/* ---- Caso "FIFO basico" ----------------------------------------------- */

/* Un productor, un consumidor, capacidad 8, 1000 mensajes. Con un solo
 * consumidor el orden de extraccion es un orden total, asi que el chequeo
 * de FIFO de ./verify es el mas estricto posible: cada mensaje tiene que
 * salir en la misma posicion en que entro. */
static bool test_fifo_basic(void) {
    return run_case("fifo", 8, 1, 1, 1000);
}

/* ---- Caso "Concurrencia" ---------------------------------------------- */

/* Cuatro productores y cuatro consumidores, capacidad 8, 1000 mensajes por
 * productor: 4000 mensajes por un buffer de 8 casillas, unas 500 vueltas
 * completas al arreglo circular. A ningun consumidor le toca un
 * subconjunto fijo de mensajes: cual extrae cada uno depende del orden en
 * que not_empty los despierte, y es justo ese reparto variable el que no
 * puede perder ni duplicar nada. */
static bool test_concurrency(void) {
    return run_case("conc", 8, 4, 4, 1000);
}

/* ---- Caso "Capacidad minima" ------------------------------------------ */

/* Capacidad 1: el caso borde del buffer circular, donde la unica casilla es
 * a la vez la que se escribe y la que se lee, y cada insercion tiene que
 * esperar a que la extraccion anterior la libere. Con 4 y 4, ocho procesos
 * se pasan el turno por ese unico lugar; si una senal se perdiera (un
 * signal donde hacia falta broadcast, D18) la corrida se quedaria clavada y
 * wait_exit lo reportaria como deadlock en vez de colgar la suite.
 *
 * Misma corrida que el caso de concurrencia (4 y 4, 1000 mensajes por
 * productor) para que la unica variable entre los dos sea la capacidad. */
static bool test_minimum_capacity(void) {
    return run_case("cap1", 1, 4, 4, 1000);
}

/* ---- Caso "Estres" ---------------------------------------------------- */

/* 32 productores y 32 consumidores, el minimo que pide el enunciado y la
 * mitad del limite de 64 casillas por rol (D8), con pausas desactivadas y
 * cuota finita: 64 procesos corriendo a toda velocidad contra un buffer de
 * 8 casillas en una maquina de 16 nucleos, o sea cuatro veces mas procesos
 * que CPU. Las cuotas son finitas para que la corrida termine sola y el
 * criterio "sin deadlock" se pueda medir: si un solo proceso quedara sin
 * despertar, su wait_exit vence a los 30 s y el caso falla nombrandolo.
 *
 * La verificacion es la completa: ./verify cruza los 65 CSV como un solo
 * conjunto de eventos. */
static bool test_stress(void) {
    return run_case("stress", 8, MAX_SIDE, MAX_SIDE, 1000);
}

/* Primero los hijos y despues los nombres: un consumidor vivo con el
 * segmento mapeado no impide el shm_unlink, pero si se van en ese orden el
 * proceso no deja nada atras en ningun camino de salida. */
static void cleanup_all(void) {
    kill_live_children();
    unlink_all_names();
}

static void on_fatal_signal(int sig) {
    (void)sig;
    cleanup_all();
    _exit(1);
}

int main(void) {
    if (access("./producer", X_OK) != 0 || access("./consumer", X_OK) != 0 ||
        access("./verify", X_OK) != 0) {
        fprintf(stderr, "test_concurrency: faltan ./producer, ./consumer o ./verify "
                        "(correr desde la raiz con make test)\n");
        return 1;
    }
    atexit(cleanup_all);
    /* El watchdog, un Ctrl-C o un `| head` que cierra la tuberia no pasan
     * por atexit: la accion por defecto de SIGALRM, SIGINT, SIGTERM y
     * SIGPIPE termina el proceso sin correr los handlers y dejaria hijos
     * bloqueados y segmentos en /dev/shm. kill(), waitpid(), shm_unlink()
     * y _exit() son async-signal-safe, asi que la misma limpieza sirve
     * desde el handler. */
    signal(SIGALRM, on_fatal_signal);
    signal(SIGINT, on_fatal_signal);
    signal(SIGTERM, on_fatal_signal);
    signal(SIGPIPE, on_fatal_signal);
    alarm(180);   /* red de seguridad: los plazos por proceso son de 30 s */
    struct test_case tests[] = {
        {"fifo_basic", test_fifo_basic},
        {"concurrency", test_concurrency},
        {"minimum_capacity", test_minimum_capacity},
        {"stress", test_stress},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
