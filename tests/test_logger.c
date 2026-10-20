/* test_logger.c — pruebas del logger CSV (D12, D16, Issue #6). Las filas
 * esperadas están escritas a mano a partir de la tabla de eventos de D12.
 * Los archivos temporales van en build/, que make test ya creó.
 */
#include "logger.h"
#include "memory.h"
#include "sync.h"
#include "test_util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Crea un archivo temporal vacío y devuelve su ruta (estática). */
static const char *temp_path(void) {
    static char path[64];
    strcpy(path, "build/test_logger_XXXXXX");
    int fd = mkstemp(path);
    if (fd == -1)
        return NULL;
    close(fd);
    return path;
}

/* Lee el archivo completo en un buffer terminado en '\0' (hay que liberar). */
static char *slurp(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (buf != NULL && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    if (buf != NULL)
        buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Registro con TODOS los campos llenos, para comprobar que el logger
 * deja vacíos los que D12 no define para cada evento. */
static struct csv_record full(enum csv_event ev) {
    struct csv_record r;
    memset(&r, 0, sizeof(r));
    r.event = ev;
    r.t_ns = 1000;
    r.producer_id = 2;
    r.sequence = 3;
    r.enqueue_seq = 4;
    r.dequeue_seq = 5;
    r.enqueue_time_ns = 900;
    r.occupancy = 6;
    r.checksum = 0xabcd;
    r.payload_ok = true;
    csv_record_set_detail(&r, "dato");
    return r;
}

#define ROW_IS(role, id, ev, expected) \
    do { \
        char b_[512]; \
        struct csv_record r_ = full(ev); \
        CHECK(csv_format_row(b_, sizeof(b_), role, id, &r_) >= 0); \
        CHECK(strcmp(b_, expected) == 0); \
    } while (0)

static bool test_rows_follow_d12(void) {
    ROW_IS(CSV_PRODUCER, 1, CSV_JOIN,          "producer,1,JOIN,1000,,,,,,,,,dato");
    ROW_IS(CSV_CONSUMER, 7, CSV_JOIN_REJECTED, "consumer,7,JOIN_REJECTED,1000,,,,,,,,,dato");
    ROW_IS(CSV_PRODUCER, 1, CSV_PUT,           "producer,1,PUT,1000,2,3,4,,900,6,0x0000abcd,,");
    ROW_IS(CSV_PRODUCER, 1, CSV_PUT_REJECTED,  "producer,1,PUT_REJECTED,1000,2,3,,,,,,,dato");
    ROW_IS(CSV_CONSUMER, 7, CSV_GET,           "consumer,7,GET,1000,2,3,4,5,900,6,0x0000abcd,1,");
    ROW_IS(CSV_CONSUMER, 7, CSV_PROCESSED,     "consumer,7,PROCESSED,1000,2,3,,5,,,,,");
    ROW_IS(CSV_CONSUMER, 7, CSV_DRAINED,       "consumer,7,DRAINED,1000,,,,,,6,,,");
    ROW_IS(CSV_PRODUCER, 1, CSV_LEAVE,         "producer,1,LEAVE,1000,,,,,,,,,dato");
    ROW_IS(CSV_FINISHER, 0, CSV_CLOSE_REQUEST, "finisher,0,CLOSE_REQUEST,1000,,,,,,,,,");
    ROW_IS(CSV_FINISHER, 0, CSV_CLOSE_REJECTED,"finisher,0,CLOSE_REJECTED,1000,,,,,,,,,dato");
    ROW_IS(CSV_FINISHER, 0, CSV_CLOSING,       "finisher,0,CLOSING,1000,,,4,5,,6,,,");
    ROW_IS(CSV_FINISHER, 0, CSV_ALL_LEFT,      "finisher,0,ALL_LEFT,1000,,,,,,,,,");
    ROW_IS(CSV_FINISHER, 0, CSV_CLOSED,        "finisher,0,CLOSED,1000,,,,,,,,,dato");
    return true;
}

static bool test_every_row_has_13_columns(void) {
    for (int ev = 0; ev < CSV_EVENT_COUNT; ev++)
        for (int role = CSV_PRODUCER; role <= CSV_FINISHER; role++) {
            char b[512];
            struct csv_record r = full((enum csv_event)ev);
            if (csv_format_row(b, sizeof(b), (enum csv_role)role, 1, &r) < 0)
                continue;           /* combinación que D12 no permite */
            int commas = 0;
            for (char *p = b; *p; p++)
                commas += *p == ',';
            CHECK(commas == 12);
        }
    return true;
}

static bool test_role_mismatch_rejected(void) {
    char b[512];
    struct csv_record r = full(CSV_PUT);
    CHECK(csv_format_row(b, sizeof(b), CSV_CONSUMER, 1, &r) == -1);   /* consumidor no inserta */
    r = full(CSV_GET);
    CHECK(csv_format_row(b, sizeof(b), CSV_PRODUCER, 1, &r) == -1);
    r = full(CSV_CLOSING);
    CHECK(csv_format_row(b, sizeof(b), CSV_CONSUMER, 1, &r) == -1);
    r = full(CSV_JOIN);
    CHECK(csv_format_row(b, sizeof(b), CSV_FINISHER, 0, &r) == -1);
    r.event = CSV_EVENT_COUNT;                                         /* fuera de rango */
    CHECK(csv_format_row(b, sizeof(b), CSV_PRODUCER, 1, &r) == -1);
    return true;
}

static bool test_buffer_too_small(void) {
    char b[16];
    struct csv_record r = full(CSV_GET);
    CHECK(csv_format_row(b, sizeof(b), CSV_CONSUMER, 1, &r) == -1);
    return true;
}

static bool test_detail_sanitized_and_truncated(void) {
    struct csv_record r;
    csv_record_event(&r, CSV_LEAVE, 5, "a,b\nc\td");
    CHECK(strcmp(r.detail, "a_b_c_d") == 0);

    char longtext[100];
    memset(longtext, 'x', sizeof(longtext) - 1);
    longtext[99] = '\0';
    csv_record_event(&r, CSV_LEAVE, 5, longtext);
    CHECK(strlen(r.detail) == CSV_DETAIL_MAX - 1);

    csv_record_event(&r, CSV_LEAVE, 5, NULL);
    CHECK(r.detail[0] == '\0');

    /* Un registro llenado a mano, sin pasar por set_detail, también sale
     * saneado. */
    char b[512];
    r = full(CSV_LEAVE);
    strcpy(r.detail, "x,y");
    CHECK(csv_format_row(b, sizeof(b), CSV_PRODUCER, 1, &r) >= 0);
    CHECK(strcmp(b, "producer,1,LEAVE,1000,,,,,,,,,x_y") == 0);
    return true;
}

static bool test_record_from_snapshot(void) {
    struct message m;
    memset(&m, 0, sizeof(m));
    m.producer_id = 3;
    m.sequence = 17;
    m.enqueue_seq = 41;
    m.enqueue_time_ns = 5000;
    m.payload.checksum = 0x12345678;

    struct op_snapshot put_snap = { .seq = 41, .time_ns = 5000, .occupancy = 2 };
    struct csv_record r;
    csv_record_put(&r, &m, &put_snap);
    char b[512];
    CHECK(csv_format_row(b, sizeof(b), CSV_PRODUCER, 3, &r) >= 0);
    CHECK(strcmp(b, "producer,3,PUT,5000,3,17,41,,5000,2,0x12345678,,") == 0);

    struct op_snapshot get_snap = { .seq = 40, .time_ns = 7500, .occupancy = 1 };
    csv_record_get(&r, &m, &get_snap, false);
    CHECK(csv_format_row(b, sizeof(b), CSV_CONSUMER, 9, &r) >= 0);
    CHECK(strcmp(b, "consumer,9,GET,7500,3,17,41,40,5000,1,0x12345678,0,") == 0);
    return true;
}

static bool test_write_n_events(void) {
    enum { N = 1000 };
    const char *path = temp_path();
    CHECK(path != NULL);

    struct csv_logger lg;
    CHECK(csv_logger_open(&lg, path, CSV_PRODUCER, 4) == 0);
    for (int i = 1; i <= N; i++) {
        struct message m;
        memset(&m, 0, sizeof(m));
        m.producer_id = 4;
        m.sequence = (uint64_t)i;
        m.enqueue_time_ns = 1000 + (uint64_t)i;
        m.payload.checksum = (uint32_t)i;
        struct op_snapshot s = { .seq = (uint64_t)i, .time_ns = 1000 + (uint64_t)i,
                                 .occupancy = (uint32_t)(i % 8) };
        struct csv_record r;
        csv_record_put(&r, &m, &s);
        CHECK(csv_logger_write(&lg, &r) == 0);
    }
    CHECK(csv_logger_close(&lg) == 0);
    CHECK(lg.f == NULL);
    CHECK(csv_logger_close(&lg) == 0);      /* cerrar dos veces no rompe */

    char *text = slurp(path);
    CHECK(text != NULL);
    char *save = NULL;
    char *line = strtok_r(text, "\n", &save);
    bool ok = line != NULL && strcmp(line, CSV_HEADER) == 0;
    int rows = 0;
    while (ok && (line = strtok_r(NULL, "\n", &save)) != NULL) {
        rows++;
        char expected[128];
        snprintf(expected, sizeof(expected),
                 "producer,4,PUT,%d,4,%d,%d,,%d,%d,0x%08x,,",
                 1000 + rows, rows, rows, 1000 + rows, rows % 8, (unsigned)rows);
        if (strcmp(line, expected) != 0)
            ok = false;
    }
    free(text);
    unlink(path);
    CHECK(ok);
    CHECK(rows == N);
    return true;
}

static bool test_wrong_event_writes_nothing(void) {
    const char *path = temp_path();
    CHECK(path != NULL);
    struct csv_logger lg;
    CHECK(csv_logger_open(&lg, path, CSV_CONSUMER, 1) == 0);
    struct csv_record r = full(CSV_PUT);
    CHECK(csv_logger_write(&lg, &r) == -EINVAL);
    CHECK(csv_logger_close(&lg) == 0);

    char *text = slurp(path);
    unlink(path);
    CHECK(text != NULL);
    bool only_header = strcmp(text, CSV_HEADER "\n") == 0;
    free(text);
    CHECK(only_header);
    return true;
}

static bool test_open_failure(void) {
    struct csv_logger lg;
    CHECK(csv_logger_open(&lg, "build/no_existe/x.csv", CSV_PRODUCER, 1) == -ENOENT);
    CHECK(lg.f == NULL);
    CHECK(csv_logger_close(&lg) == 0);
    return true;
}

int main(void) {
    struct test_case tests[] = {
        {"rows_follow_d12", test_rows_follow_d12},
        {"every_row_has_13_columns", test_every_row_has_13_columns},
        {"role_mismatch_rejected", test_role_mismatch_rejected},
        {"buffer_too_small", test_buffer_too_small},
        {"detail_sanitized_and_truncated", test_detail_sanitized_and_truncated},
        {"record_from_snapshot", test_record_from_snapshot},
        {"write_n_events", test_write_n_events},
        {"wrong_event_writes_nothing", test_wrong_event_writes_nothing},
        {"open_failure", test_open_failure},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
