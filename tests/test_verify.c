/* test_verify.c — pruebas del parseo de CSV (Issue #7, slice 1: solo
 * log_row_parse/log_rows_read; la logica de cruce/verificacion llega en
 * slices siguientes y se prueba aparte.
 */
#include "verify.h"
#include "test_util.h"

#include <stdlib.h>
#include <string.h>

#define CSV_HEADER \
    "role,id,event,t_ns,producer_id,sequence,enqueue_seq,dequeue_seq," \
    "enqueue_time_ns,occupancy,checksum,payload_ok,detail"

/* Fila PUT valida, campo por campo y en el orden de D12. Las pruebas que
 * verifican el rechazo de un valor copian este arreglo y pisan una sola
 * columna, asi queda claro que lo unico malo de la fila es ese campo. */
static const char *valid_fields[13] = {
    "producer", "1", "PUT", "1000", "1", "1", "1", "",
    "1000", "1", "0x9c3a51e2", "", ""
};

/* Une 13 campos con comas. El conteo de comas lo garantiza el formato
 * (12 fijas), no la aritmetica de quien escribe la prueba. */
static void build_row(char *buf, size_t size, const char **f) {
    snprintf(buf, size, "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s",
             f[0], f[1], f[2], f[3], f[4], f[5], f[6],
             f[7], f[8], f[9], f[10], f[11], f[12]);
}

/* true si la fila valida, con `value` en la columna `col`, se rechaza. */
static bool rejects_with(size_t col, const char *value) {
    const char *f[13];
    memcpy(f, valid_fields, sizeof(f));
    f[col] = value;

    char line[1024];
    build_row(line, sizeof(line), f);

    struct log_row row;
    return !log_row_parse(line, &row);
}

/* Filas de ejemplo tal cual aparecen en D12 (docs/decisiones_diseno.md). */
static bool test_parse_put_row(void) {
    struct log_row row;
    CHECK(log_row_parse(
        "producer,1,PUT,81234567000,1,1,1,,81234567000,1,0x9c3a51e2,,",
        &row));
    CHECK(row.role == LOG_ROLE_PRODUCER);
    CHECK(row.id == 1);
    CHECK(row.event == LOG_EVENT_PUT);
    CHECK(row.t_ns == 81234567000ull);
    CHECK(row.producer_id == 1);
    CHECK(row.sequence == 1);
    CHECK(row.enqueue_seq == 1);
    CHECK(row.dequeue_seq == 0);   /* vacio -> 0, PUT no lo lleva */
    CHECK(row.enqueue_time_ns == 81234567000ull);
    CHECK(row.occupancy == 1);
    CHECK(row.checksum == 0x9c3a51e2u);
    CHECK(row.payload_ok == 0);    /* vacio -> 0, PUT no lo lleva */
    CHECK(row.detail[0] == '\0');
    return true;
}

static bool test_parse_get_row(void) {
    struct log_row row;
    CHECK(log_row_parse(
        "consumer,1,GET,81234612000,1,1,1,1,81234567000,0,0x9c3a51e2,1,",
        &row));
    CHECK(row.role == LOG_ROLE_CONSUMER);
    CHECK(row.event == LOG_EVENT_GET);
    CHECK(row.dequeue_seq == 1);
    CHECK(row.occupancy == 0);
    CHECK(row.payload_ok == 1);
    return true;
}

/* DRAINED solo lleva `occupancy` (D12); todo el resto de los campos
 * opcionales va vacio. */
static bool test_parse_most_fields_empty(void) {
    const char *f[13] = {"consumer", "3", "DRAINED", "2000",
                         "", "", "", "", "", "5", "", "", ""};
    char line[256];
    build_row(line, sizeof(line), f);

    struct log_row row;
    CHECK(log_row_parse(line, &row));
    CHECK(row.role == LOG_ROLE_CONSUMER);
    CHECK(row.id == 3);
    CHECK(row.event == LOG_EVENT_DRAINED);
    CHECK(row.t_ns == 2000);
    CHECK(row.producer_id == 0);
    CHECK(row.sequence == 0);
    CHECK(row.enqueue_seq == 0);
    CHECK(row.dequeue_seq == 0);
    CHECK(row.enqueue_time_ns == 0);
    CHECK(row.occupancy == 5);
    CHECK(row.checksum == 0);
    CHECK(row.payload_ok == 0);
    CHECK(row.detail[0] == '\0');
    return true;
}

/* JOIN lleva el pid del proceso como texto en `detail` (D12). */
static bool test_parse_detail_field(void) {
    const char *f[13] = {"producer", "5", "JOIN", "1000",
                         "", "", "", "", "", "", "", "", "12345"};
    char line[256];
    build_row(line, sizeof(line), f);

    struct log_row row;
    CHECK(log_row_parse(line, &row));
    CHECK(strcmp(row.detail, "12345") == 0);
    return true;
}

static bool test_parse_rejects_malformed(void) {
    struct log_row row;

    /* menos de 13 columnas */
    CHECK(!log_row_parse("producer,1,PUT,1000", &row));

    /* mas de 13 columnas */
    CHECK(!log_row_parse(
        "producer,1,PUT,1000,1,1,1,,1000,1,0x1,,,extra", &row));

    /* role desconocido */
    CHECK(rejects_with(0, "worker"));

    /* event desconocido */
    CHECK(rejects_with(2, "PUSH"));

    /* campo numerico con texto */
    CHECK(rejects_with(3, "abc"));

    /* payload_ok invalido (solo 0/1) */
    CHECK(rejects_with(11, "2"));

    /* checksum con texto no hexadecimal */
    CHECK(rejects_with(10, "0xZZ"));

    /* campos obligatorios vacios: id y t_ns van en toda fila (D12) */
    CHECK(rejects_with(1, ""));
    CHECK(rejects_with(3, ""));

    return true;
}

/* strtoull/strtoul aceptan un signo delante y hacen wraparound sin marcar
 * ERANGE: "-1" en base 16 devuelve ULONG_MAX. Ninguna columna de D12 es
 * negativa, asi que el signo se corta antes de convertir. */
static bool test_parse_rejects_signed_numbers(void) {
    CHECK(rejects_with(3, "-1"));          /* t_ns */
    CHECK(rejects_with(4, "+1"));          /* producer_id */
    CHECK(rejects_with(10, "-1"));         /* checksum, base 16 */
    /* En 64 bits "-1" ya lo atrapa el limite de 32 bits (da ULONG_MAX);
     * estos dos solo los rechaza el corte por el signo. */
    CHECK(rejects_with(10, "-0"));
    CHECK(rejects_with(10, "+1"));
    return true;
}

static bool test_parse_rejects_numeric_overflow(void) {
    /* producer_id es uint32_t: entra en uint64_t pero no en su columna. */
    CHECK(rejects_with(4, "5000000000"));
    /* occupancy, mismo caso */
    CHECK(rejects_with(9, "4294967296"));
    /* t_ns es uint64_t: esto desborda hasta ahi (ERANGE) */
    CHECK(rejects_with(3, "99999999999999999999999"));
    /* checksum no entra en 32 bits */
    CHECK(rejects_with(10, "0x1ffffffff"));
    return true;
}

/* El limite exacto de `detail`: LOG_DETAIL_MAX incluye el '\0', asi que
 * entran 47 caracteres y el 48 se rechaza en vez de truncarse. */
static bool test_parse_detail_length_boundary(void) {
    char detail[LOG_DETAIL_MAX + 1];

    memset(detail, 'd', LOG_DETAIL_MAX - 1);
    detail[LOG_DETAIL_MAX - 1] = '\0';
    CHECK(!rejects_with(12, detail));   /* 47: entra justo */

    memset(detail, 'd', LOG_DETAIL_MAX);
    detail[LOG_DETAIL_MAX] = '\0';
    CHECK(rejects_with(12, detail));    /* 48: se rechaza */
    return true;
}

/* Una linea que no entra en el buffer de lectura se rechaza entera. */
static bool test_parse_rejects_long_line(void) {
    char huge[700];
    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    CHECK(rejects_with(12, huge));
    return true;
}

static bool test_rows_read_from_memory(void) {
    static const char csv[] =
        CSV_HEADER "\n"
        "producer,1,PUT,1000,1,1,1,,1000,1,0x9c3a51e2,,\n"
        "consumer,1,GET,2000,1,1,1,1,1000,0,0x9c3a51e2,1,\n";

    FILE *f = fmemopen((void *)csv, strlen(csv), "r");
    CHECK(f != NULL);

    struct log_row *rows = NULL;
    size_t count = 0;
    bool ok = log_rows_read(f, &rows, &count);
    fclose(f);

    CHECK(ok);
    CHECK(count == 2);
    CHECK(rows[0].event == LOG_EVENT_PUT);
    CHECK(rows[1].event == LOG_EVENT_GET);
    free(rows);
    return true;
}

static bool test_rows_read_header_only(void) {
    static const char csv[] = CSV_HEADER "\n";

    FILE *f = fmemopen((void *)csv, strlen(csv), "r");
    CHECK(f != NULL);

    struct log_row *rows = NULL;
    size_t count = 0;
    bool ok = log_rows_read(f, &rows, &count);
    fclose(f);

    CHECK(ok);
    CHECK(count == 0);
    CHECK(rows == NULL);
    return true;
}

static bool test_rows_read_rejects_empty_file(void) {
    static const char csv[] = "";

    FILE *f = fmemopen((void *)csv, 0, "r");
    CHECK(f != NULL);

    struct log_row *rows = NULL;
    size_t count = 0;
    bool ok = log_rows_read(f, &rows, &count);
    fclose(f);

    CHECK(!ok);
    CHECK(rows == NULL);
    CHECK(count == 0);
    return true;
}

static bool test_rows_read_rejects_malformed_row(void) {
    static const char csv[] =
        CSV_HEADER "\n"
        "producer,1,PUT,1000,1,1,1,,1000,1,0x9c3a51e2,,\n"
        "producer,1,PUT,1000\n";   /* le faltan columnas */

    FILE *f = fmemopen((void *)csv, strlen(csv), "r");
    CHECK(f != NULL);

    struct log_row *rows = NULL;
    size_t count = 0;
    bool ok = log_rows_read(f, &rows, &count);
    fclose(f);

    CHECK(!ok);
    CHECK(rows == NULL);
    CHECK(count == 0);
    return true;
}

/* Un archivo escrito en Windows (CRLF) se lee igual: el '\r' no puede
 * quedar pegado al ultimo campo. */
static bool test_rows_read_handles_crlf(void) {
    static const char csv[] =
        CSV_HEADER "\r\n"
        "producer,1,PUT,1000,1,1,1,,1000,1,0x9c3a51e2,,\r\n"
        "producer,1,LEAVE,2000,,,,,,,,,quota_done\r\n";

    FILE *f = fmemopen((void *)csv, strlen(csv), "r");
    CHECK(f != NULL);

    struct log_row *rows = NULL;
    size_t count = 0;
    bool ok = log_rows_read(f, &rows, &count);
    fclose(f);

    CHECK(ok);
    CHECK(count == 2);
    CHECK(rows[0].checksum == 0x9c3a51e2u);
    CHECK(strcmp(rows[1].detail, "quota_done") == 0);   /* sin '\r' pegado */
    free(rows);
    return true;
}

static bool test_rows_read_skips_blank_lines(void) {
    static const char csv[] =
        CSV_HEADER "\n"
        "producer,1,PUT,1000,1,1,1,,1000,1,0x9c3a51e2,,\n"
        "\n";

    FILE *f = fmemopen((void *)csv, strlen(csv), "r");
    CHECK(f != NULL);

    struct log_row *rows = NULL;
    size_t count = 0;
    bool ok = log_rows_read(f, &rows, &count);
    fclose(f);

    CHECK(ok);
    CHECK(count == 1);
    free(rows);
    return true;
}

/* Un proceso que muere antes de cerrar el archivo puede dejar la ultima
 * fila completa pero sin '\n'. Esa fila es valida y debe contarse. */
static bool test_rows_read_last_row_without_newline(void) {
    static const char csv[] =
        CSV_HEADER "\n"
        "producer,1,PUT,1000,1,1,1,,1000,1,0x9c3a51e2,,\n"
        "producer,1,PUT,2000,1,2,2,,2000,1,0x9c3a51e2,,";   /* sin '\n' */

    FILE *f = fmemopen((void *)csv, strlen(csv), "r");
    CHECK(f != NULL);

    struct log_row *rows = NULL;
    size_t count = 0;
    bool ok = log_rows_read(f, &rows, &count);
    fclose(f);

    CHECK(ok);
    CHECK(count == 2);
    CHECK(rows[1].sequence == 2);
    free(rows);
    return true;
}

/* Mas de 64 filas obliga al segundo realloc (cap 0 -> 64 -> 128), que es
 * donde el bloque puede cambiar de direccion. */
static bool test_rows_read_grows_beyond_initial_capacity(void) {
    enum { ROWS = 100 };
    char csv[16384];

    int n = snprintf(csv, sizeof(csv), "%s\n", CSV_HEADER);
    for (unsigned i = 1; i <= ROWS; i++) {
        n += snprintf(csv + n, sizeof(csv) - (size_t)n,
                      "producer,1,PUT,%u,1,%u,%u,,%u,1,0x1,,\n", i, i, i, i);
    }
    CHECK(n > 0 && (size_t)n < sizeof(csv));   /* no se trunco al armarlo */

    FILE *f = fmemopen(csv, strlen(csv), "r");
    CHECK(f != NULL);

    struct log_row *rows = NULL;
    size_t count = 0;
    bool ok = log_rows_read(f, &rows, &count);
    fclose(f);

    CHECK(ok);
    CHECK(count == ROWS);
    CHECK(rows[0].sequence == 1);
    CHECK(rows[ROWS - 1].sequence == ROWS);   /* sobrevivio al realloc */
    free(rows);
    return true;
}

/* El caso peligroso de una fila que no entra en el buffer: fgets la
 * entrega en dos pedazos y, si ambos parsean, el verificador contaria una
 * fila que nunca se escribio (un PUT duplicado de la nada). Se arma a
 * proposito: un prefijo de exactamente LOG_LINE_MAX-1 caracteres que es
 * una fila valida — rellenando t_ns con ceros a la izquierda, que siguen
 * siendo digitos — pegado a otra fila valida, sin salto de linea en
 * medio. */
static bool test_rows_read_rejects_split_line(void) {
    static const char head[] = "producer,1,PUT,";
    static const char tail[] = ",1,1,1,,1000,1,0x1,,";

    size_t pad_len = (LOG_LINE_MAX - 1) - strlen(head) - strlen(tail)
                     - strlen("1000");
    char pad[LOG_LINE_MAX];
    memset(pad, '0', pad_len);
    pad[pad_len] = '\0';

    char first[LOG_LINE_MAX];
    int n = snprintf(first, sizeof(first), "%s%s1000%s", head, pad, tail);
    CHECK(n > 0 && (size_t)n == LOG_LINE_MAX - 1);   /* llena el buffer justo */

    /* El prefijo, por si solo, es una fila perfectamente valida: sin la
     * deteccion de linea partida, log_rows_read la aceptaria. */
    struct log_row probe;
    CHECK(log_row_parse(first, &probe));
    CHECK(probe.t_ns == 1000);

    char csv[2048];
    n = snprintf(csv, sizeof(csv), "%s\n%s%s\n", CSV_HEADER, first,
                 "producer,2,PUT,2000,2,1,2,,2000,1,0x1,,");
    CHECK(n > 0 && (size_t)n < sizeof(csv));

    FILE *f = fmemopen(csv, strlen(csv), "r");
    CHECK(f != NULL);

    struct log_row *rows = NULL;
    size_t count = 0;
    bool ok = log_rows_read(f, &rows, &count);
    fclose(f);

    CHECK(!ok);
    CHECK(rows == NULL);
    CHECK(count == 0);
    return true;
}

/* ---- Conservacion de identidades (D15, chequeo 1) -------------------- */

static struct log_row put_row(uint32_t producer_id, uint64_t sequence) {
    struct log_row r = {0};
    r.role = LOG_ROLE_PRODUCER;
    r.id = producer_id;
    r.event = LOG_EVENT_PUT;
    r.producer_id = producer_id;
    r.sequence = sequence;
    return r;
}

static struct log_row get_row(uint32_t producer_id, uint64_t sequence) {
    struct log_row r = {0};
    r.role = LOG_ROLE_CONSUMER;
    r.id = 9;
    r.event = LOG_EVENT_GET;
    r.producer_id = producer_id;
    r.sequence = sequence;
    return r;
}

static struct log_row event_row(enum log_event event, uint32_t producer_id,
                                uint64_t sequence) {
    struct log_row r = {0};
    r.role = LOG_ROLE_PRODUCER;
    r.id = producer_id;
    r.event = event;
    r.producer_id = producer_id;
    r.sequence = sequence;
    return r;
}

/* Una corrida sana: todo lo insertado se extrajo una sola vez, y los
 * eventos que no son PUT/GET no estorban. */
static bool test_identities_clean_run(void) {
    struct log_row rows[] = {
        event_row(LOG_EVENT_JOIN, 1, 0),
        put_row(1, 1), put_row(1, 2), put_row(2, 1),
        get_row(2, 1), get_row(1, 1), get_row(1, 2),   /* desordenados a proposito */
        event_row(LOG_EVENT_DRAINED, 0, 0),
        event_row(LOG_EVENT_LEAVE, 1, 0),
    };

    struct verify_report rep = {0};
    CHECK(verify_identities(rows, sizeof(rows) / sizeof(rows[0]), &rep));
    CHECK(rep.count == 0);
    verify_report_free(&rep);
    return true;
}

static bool test_identities_detects_loss(void) {
    struct log_row rows[] = {
        put_row(1, 1), put_row(1, 2),
        get_row(1, 1),   /* el 2 nunca sale */
    };

    struct verify_report rep = {0};
    CHECK(verify_identities(rows, sizeof(rows) / sizeof(rows[0]), &rep));
    CHECK(rep.count == 1);
    CHECK(rep.items[0].kind == VIOLATION_LOST);
    CHECK(rep.items[0].producer_id == 1);
    CHECK(rep.items[0].sequence == 2);
    CHECK(rep.items[0].gets == 0);
    verify_report_free(&rep);
    return true;
}

static bool test_identities_detects_duplicate(void) {
    struct log_row rows[] = {
        put_row(3, 7),
        get_row(3, 7), get_row(3, 7), get_row(3, 7),
    };

    struct verify_report rep = {0};
    CHECK(verify_identities(rows, sizeof(rows) / sizeof(rows[0]), &rep));
    CHECK(rep.count == 1);
    CHECK(rep.items[0].kind == VIOLATION_DUPLICATED);
    CHECK(rep.items[0].producer_id == 3);
    CHECK(rep.items[0].sequence == 7);
    CHECK(rep.items[0].gets == 3);
    verify_report_free(&rep);
    return true;
}

static bool test_identities_detects_phantom(void) {
    struct log_row rows[] = {
        put_row(1, 1), get_row(1, 1),
        get_row(4, 9),   /* nadie lo inserto */
    };

    struct verify_report rep = {0};
    CHECK(verify_identities(rows, sizeof(rows) / sizeof(rows[0]), &rep));
    CHECK(rep.count == 1);
    CHECK(rep.items[0].kind == VIOLATION_PHANTOM);
    CHECK(rep.items[0].producer_id == 4);
    CHECK(rep.items[0].sequence == 9);
    verify_report_free(&rep);
    return true;
}

/* Dos PUT con la misma identidad: aunque las extracciones cuadren en
 * cantidad, el productor violo la unicidad de (producer_id, sequence). */
static bool test_identities_detects_repeated_id(void) {
    struct log_row rows[] = {
        put_row(1, 1), put_row(1, 1),
        get_row(1, 1), get_row(1, 1),
    };

    struct verify_report rep = {0};
    CHECK(verify_identities(rows, sizeof(rows) / sizeof(rows[0]), &rep));
    CHECK(rep.count == 1);
    CHECK(rep.items[0].kind == VIOLATION_REPEATED_ID);
    verify_report_free(&rep);
    return true;
}

/* PUT_REJECTED lleva identidad pero el mensaje nunca entro al buffer:
 * contarlo como insertado convertiria cada rechazo en una perdida. */
static bool test_identities_ignores_rejected_put(void) {
    struct log_row rows[] = {
        put_row(1, 1), get_row(1, 1),
        event_row(LOG_EVENT_PUT_REJECTED, 1, 2),
    };

    struct verify_report rep = {0};
    CHECK(verify_identities(rows, sizeof(rows) / sizeof(rows[0]), &rep));
    CHECK(rep.count == 0);
    verify_report_free(&rep);
    return true;
}

/* Varias violaciones a la vez: se reportan todas, ordenadas por
 * (producer_id, sequence). */
static bool test_identities_reports_all_sorted(void) {
    struct log_row rows[] = {
        put_row(2, 1),                      /* perdido */
        put_row(1, 5), get_row(1, 5), get_row(1, 5),   /* duplicado */
        get_row(1, 2),                      /* fantasma */
    };

    struct verify_report rep = {0};
    CHECK(verify_identities(rows, sizeof(rows) / sizeof(rows[0]), &rep));
    CHECK(rep.count == 3);

    CHECK(rep.items[0].producer_id == 1 && rep.items[0].sequence == 2);
    CHECK(rep.items[0].kind == VIOLATION_PHANTOM);
    CHECK(rep.items[1].producer_id == 1 && rep.items[1].sequence == 5);
    CHECK(rep.items[1].kind == VIOLATION_DUPLICATED);
    CHECK(rep.items[2].producer_id == 2 && rep.items[2].sequence == 1);
    CHECK(rep.items[2].kind == VIOLATION_LOST);

    verify_report_free(&rep);
    return true;
}

/* El reporte acumula: cada chequeo agrega lo suyo sin pisar lo anterior. */
static bool test_report_accumulates(void) {
    struct log_row first[] = {put_row(1, 1)};
    struct log_row second[] = {get_row(2, 2)};

    struct verify_report rep = {0};
    CHECK(verify_identities(first, 1, &rep));
    CHECK(rep.count == 1);
    CHECK(verify_identities(second, 1, &rep));
    CHECK(rep.count == 2);
    CHECK(rep.items[0].kind == VIOLATION_LOST);
    CHECK(rep.items[1].kind == VIOLATION_PHANTOM);

    verify_report_free(&rep);
    CHECK(rep.count == 0);
    CHECK(rep.items == NULL);
    return true;
}

static bool test_identities_empty_input(void) {
    struct verify_report rep = {0};
    CHECK(verify_identities(NULL, 0, &rep));
    CHECK(rep.count == 0);

    /* Un log sin PUT ni GET tampoco es violacion. */
    struct log_row rows[] = {event_row(LOG_EVENT_JOIN, 1, 0)};
    CHECK(verify_identities(rows, 1, &rep));
    CHECK(rep.count == 0);

    verify_report_free(&rep);
    return true;
}

/* Las dos capas juntas: CSV crudo -> log_rows_read -> verify_identities,
 * con dos fallas inyectadas y un PUT_REJECTED que no debe contar. */
static bool test_end_to_end_csv_to_violations(void) {
    static const char csv[] =
        CSV_HEADER "\n"
        "producer,1,JOIN,1000,,,,,,,,,4321\n"
        "producer,1,PUT,1100,1,1,1,,1100,1,0xaaaa0001,,\n"
        "producer,2,PUT,1200,2,1,2,,1200,2,0xaaaa0002,,\n"
        "producer,1,PUT,1300,1,2,3,,1300,3,0xaaaa0003,,\n"
        "producer,1,PUT,1400,1,3,4,,1400,4,0xaaaa0004,,\n"   /* nunca sale */
        "producer,1,PUT_REJECTED,1450,1,4,,,,,,,closing\n"
        "consumer,9,GET,1500,1,1,1,1,1100,3,0xaaaa0001,1,\n"
        "consumer,9,GET,1600,2,1,2,2,1200,2,0xaaaa0002,1,\n"
        "consumer,8,GET,1700,2,1,2,3,1200,1,0xaaaa0002,1,\n"  /* sale 2 veces */
        "consumer,9,GET,1800,1,2,3,4,1300,0,0xaaaa0003,1,\n"
        "consumer,9,DRAINED,1900,,,,,,0,,,\n";

    FILE *f = fmemopen((void *)csv, strlen(csv), "r");
    CHECK(f != NULL);

    struct log_row *rows = NULL;
    size_t n = 0;
    bool read_ok = log_rows_read(f, &rows, &n);
    fclose(f);
    CHECK(read_ok);
    CHECK(n == 11);

    struct verify_report rep = {0};
    CHECK(verify_identities(rows, n, &rep));
    CHECK(rep.count == 2);

    CHECK(rep.items[0].kind == VIOLATION_LOST);
    CHECK(rep.items[0].producer_id == 1 && rep.items[0].sequence == 3);
    CHECK(rep.items[1].kind == VIOLATION_DUPLICATED);
    CHECK(rep.items[1].producer_id == 2 && rep.items[1].sequence == 1);
    CHECK(rep.items[1].gets == 2);

    verify_report_free(&rep);
    free(rows);
    return true;
}

/* Cada tipo tiene su mensaje, y ninguno cae en el caso desconocido. */
static bool test_violation_kind_str(void) {
    const enum violation_kind kinds[] = {
        VIOLATION_LOST, VIOLATION_DUPLICATED,
        VIOLATION_PHANTOM, VIOLATION_REPEATED_ID
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        const char *s = violation_kind_str(kinds[i]);
        CHECK(s != NULL && s[0] != '\0');
        CHECK(strstr(s, "desconocida") == NULL);
    }
    return true;
}

int main(void) {
    struct test_case tests[] = {
        {"parse_put_row", test_parse_put_row},
        {"parse_get_row", test_parse_get_row},
        {"parse_most_fields_empty", test_parse_most_fields_empty},
        {"parse_detail_field", test_parse_detail_field},
        {"parse_rejects_malformed", test_parse_rejects_malformed},
        {"parse_rejects_signed_numbers", test_parse_rejects_signed_numbers},
        {"parse_rejects_numeric_overflow", test_parse_rejects_numeric_overflow},
        {"parse_detail_length_boundary", test_parse_detail_length_boundary},
        {"parse_rejects_long_line", test_parse_rejects_long_line},
        {"rows_read_from_memory", test_rows_read_from_memory},
        {"rows_read_header_only", test_rows_read_header_only},
        {"rows_read_rejects_empty_file", test_rows_read_rejects_empty_file},
        {"rows_read_rejects_malformed_row", test_rows_read_rejects_malformed_row},
        {"rows_read_handles_crlf", test_rows_read_handles_crlf},
        {"rows_read_skips_blank_lines", test_rows_read_skips_blank_lines},
        {"rows_read_last_row_without_newline",
         test_rows_read_last_row_without_newline},
        {"rows_read_grows_beyond_initial_capacity",
         test_rows_read_grows_beyond_initial_capacity},
        {"rows_read_rejects_split_line", test_rows_read_rejects_split_line},
        {"identities_clean_run", test_identities_clean_run},
        {"identities_detects_loss", test_identities_detects_loss},
        {"identities_detects_duplicate", test_identities_detects_duplicate},
        {"identities_detects_phantom", test_identities_detects_phantom},
        {"identities_detects_repeated_id", test_identities_detects_repeated_id},
        {"identities_ignores_rejected_put", test_identities_ignores_rejected_put},
        {"identities_reports_all_sorted", test_identities_reports_all_sorted},
        {"report_accumulates", test_report_accumulates},
        {"identities_empty_input", test_identities_empty_input},
        {"end_to_end_csv_to_violations", test_end_to_end_csv_to_violations},
        {"violation_kind_str", test_violation_kind_str},
    };
    int failed = test_util_run(tests, sizeof(tests) / sizeof(tests[0]));
    return failed ? 1 : 0;
}
