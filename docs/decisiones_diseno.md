# Decisiones de diseño

Bitácora viva de decisiones de diseño del Proyecto 2 (MC6004 SOA). Cada
entrada queda numerada (D1, D2…) y no se borra aunque una decisión posterior
la reemplace — en ese caso la entrada nueva dice explícitamente a cuál
reemplaza y por qué. El objetivo es que cualquiera (incluido el profesor)
pueda reconstruir por qué el código quedó como quedó sin tener que preguntar.

---

## D1 — Layout del segmento de memoria compartida

- **Fecha:** 2026-10-14
- **Issue:** [#2](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/2) (Diseño A), slice 1
- **Quién decide:** Emmanuel · **Revisa:** Isaac

### Contexto

El enunciado fija varias restricciones sobre qué puede vivir en el segmento
compartido y cómo:

- *"La representación compartida utilizará valores, índices o
  desplazamientos; no dependerá de que una dirección virtual obtenida en un
  proceso sea válida en otro."* → nada de punteros dentro del segmento.
- *"Los mensajes tendrán tamaño fijo y no contendrán punteros a memoria
  privada."*
- Tabla "Campo mínimo" del mensaje: `producer_id` y `sequence`,
  `enqueue_seq`, `enqueue_time_ns`, `payload` (tamaño fijo, contenido
  verificable, definido por el grupo).
- *"La creación deberá ser exclusiva y la publicación de una instancia lista
  no podrá exponer estructuras parcialmente inicializadas."*
- *"Se permiten semáforos POSIX o mutex y variables de condición POSIX
  configurados para compartirse entre procesos (...) Separar
  conceptualmente exclusión del estado, disponibilidad de mensajes y
  disponibilidad de espacio."*
- *"El finalizador (...) garantiza que no quedan usuarios ni esperas sobre
  las primitivas antes de destruirlas."* → hace falta un contador de
  participantes activos.

### Decisión

Un único `struct instance`, inicializado por `creator` dentro del segmento
`mmap`, con el arreglo de mensajes como miembro flexible al final. Los
identificadores del código van en inglés; los comentarios, en español:

```c
#include <stdatomic.h>
#include <stdint.h>
#include <pthread.h>

#define INSTANCE_MAGIC      0xDEADBEEFu   /* arbitrario, != 0, fácil de ver en un hexdump/gdb */
#define INSTANCE_VERSION    1u
#define PAYLOAD_FILL_SIZE   16             /* ver sección "Payload" abajo */

enum instance_state { STATE_OPEN = 0, STATE_CLOSING = 1, STATE_CLOSED = 2 };

/* Campos ordenados de mayor a menor alineación (8, 4, 1 bytes) a propósito:
 * así sizeof(struct payload) da 32 sin relleno invisible del compilador.
 * Ver "Payload: metadata + checksum" más abajo. */
struct payload {
    uint64_t sequence;                    /* copia: ver "redundancia deliberada" */
    uint32_t producer_id;                 /* copia: ver "redundancia deliberada" */
    uint32_t checksum;                    /* FNV-1a de 32 bits sobre los campos de abajo */
    uint8_t  fill[PAYLOAD_FILL_SIZE];     /* fill[i] = (producer_id + sequence + i) % 256 */
};                                         /* sizeof == 32 bytes, sin relleno del compilador */

struct message {
    uint32_t         producer_id;
    uint64_t         sequence;             /* por productor */
    uint64_t         enqueue_seq;          /* global, asignado bajo protección, empieza en 1 */
    uint64_t         enqueue_time_ns;      /* CLOCK_MONOTONIC, capturado bajo protección */
    struct payload   payload;
};

struct instance {
    _Atomic uint32_t magic;        /* se escribe último, ver "Publicación segura" */
    uint32_t         version;
    uint32_t         capacity;     /* 1..1024, fija toda la vida de la instancia */
    int              state;        /* enum instance_state */

    pthread_mutex_t  mutex;        /* PTHREAD_PROCESS_SHARED */
    pthread_cond_t   not_full;     /* disponibilidad de espacio */
    pthread_cond_t   not_empty;    /* disponibilidad de mensajes */
    pthread_cond_t   all_left;     /* para que el finisher espere activos == 0 */

    uint32_t  head, tail, count;
    uint64_t  enqueue_seq, dequeue_seq;

    uint32_t  active_producers, active_consumers;

    /* tabla de ids usados por rol (unicidad de por vida): se agrega en #12 */

    struct message messages[];     /* capacity * sizeof(struct message), al final */
};
```

`creator` dimensiona el segmento con
`ftruncate(fd, sizeof(struct instance) + capacity * sizeof(struct message))`.

### Publicación segura (evitar exponer estado parcial)

`magic` es un entero atómico (`_Atomic uint32_t`), no uno normal. `creator`
inicializa todo el resto de la estructura primero (mutex, condvars,
contadores en 0, `capacity`, `version`) y al final hace:

```c
atomic_store_explicit(&inst->magic, INSTANCE_MAGIC, memory_order_release);
```

Cualquier otro proceso, al hacer `attach`, valida con:

```c
if (atomic_load_explicit(&inst->magic, memory_order_acquire) != INSTANCE_MAGIC) {
    /* instancia inexistente o aún no lista: error claro, sin reintento silencioso */
}
```

El par `release`/`acquire` es necesario porque el orden en que un proceso
escribe campos no está garantizado que se observe en ese mismo orden desde
otro núcleo/proceso sin una sincronización explícita. Si el segundo proceso
observa el `magic` escrito por el primero, el modelo de memoria de C
garantiza que también observa todo lo que el primero escribió *antes* de
ese `store`. Sin este par, en teoría un proceso podría ver `magic` ya
puesto y aun así leer `capacity` o `head` todavía en cero.

No se eligió un mutex para proteger la publicación inicial porque un
proceso que recién llega no puede confiar en que el mutex ya esté
inicializado — sería circular (necesitaría el mutex para saber si el mutex
existe).

### Orden de creación y deshacer parcial

El orden de los pasos del `creator` no es cosmético: es lo que garantiza
que `magic` nunca quede escrito sobre una estructura incompleta. La
implementación va en el Issue #9, pero la secuencia se fija acá.

1. Validar nombre y capacidad (1..1024). Todavía no se creó nada.
2. `shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600)` — creación
   exclusiva. Si devuelve `EEXIST`, la instancia ya existe y se reporta
   el error **sin tocarla**.
3. `ftruncate(fd, sizeof(struct instance) + capacity * sizeof(struct message))`.
4. `mmap(...)` con `MAP_SHARED`.
5. `close(fd)` — el mapeo sobrevive sin el descriptor.
6. `sync_init_primitives()`: mutex y las tres condvars con
   `PTHREAD_PROCESS_SHARED` y `CLOCK_MONOTONIC`.
7. Escribir `version`, `capacity` y `state = STATE_OPEN`. El resto de
   contadores ya vale 0: el kernel entrega el segmento lleno de ceros.
8. **Último:** `atomic_store_explicit(&inst->magic, INSTANCE_MAGIC,
   memory_order_release)`.

Qué se deshace según dónde falle, para cumplir *"los errores durante la
creación deberán deshacer únicamente los recursos creados por esa
operación"*:

| Falla en | Qué deshacer |
|---|---|
| 1 (validación) | Nada: no se creó ningún recurso. |
| 2 (`shm_open`) | Nada. Si fue `EEXIST`, **no** se hace `shm_unlink`: ese segmento es de otra instancia viva y borrarlo sería destruir trabajo ajeno. |
| 3 o 4 | `close(fd)` + `shm_unlink(name)` — el nombre sí lo creamos nosotros. |
| 6 | `sync_init_primitives` deshace las primitivas que alcanzó a inicializar; luego `munmap` + `shm_unlink`. |
| 7 | `sync_destroy_primitives` + `munmap` + `shm_unlink`. |
| Después de 8 | Nada: la instancia quedó publicada y el `creator` retorna éxito. |

Dos consecuencias que vale la pena tener claras:

- Como `magic` se escribe solo si todos los pasos anteriores salieron
  bien, **una creación abortada no puede dejar un segmento que *parezca*
  listo**. Y como además se hace `shm_unlink` al fallar, tampoco queda el
  nombre ocupado por un segmento a medio construir.
- Entre el paso 2 (el nombre ya existe) y el 8 (listo) hay una ventana en
  la que otro proceso puede abrir el segmento y leer `magic == 0`. Eso
  funciona porque el kernel entrega el segmento en ceros y `0 !=
  INSTANCE_MAGIC` — por eso la constante tiene que ser distinta de cero.
  Ese proceso falla con un error claro; no reintenta ni espera, porque el
  enunciado no pide que un participante sobreviva a un `creator` que
  todavía no terminó.

### Payload: metadata + checksum

Contenido elegido para que sea "verificable" (exigencia explícita del
enunciado) sin que el verificador necesite guardar nada aparte del propio
CSV:

1. `producer_id` y `sequence` copiados dentro del payload — permite
   comparar "lo que dice el sobre del mensaje" contra "lo que dice el
   payload" como chequeo cruzado independiente.
2. `fill[16]` determinista: `fill[i] = (producer_id + sequence + i) %
   256`. El verificador lo puede recalcular solo con `producer_id` y
   `sequence`.
3. `checksum`: FNV-1a de 32 bits sobre los bytes lógicos de `sequence` (8)
   + `producer_id` (4) + `fill` (16) = 28 bytes de entrada, **serializados
   campo por campo explícitamente** — nunca haciendo un hash directo sobre
   `&payload`/`sizeof(payload)`, para no depender de cómo decida alinear
   los campos el compilador (ver nota de alineación en el bloque de
   código). En el orden de los campos de la máquina local; la ejecución es
   de un solo host, así que no hace falta normalizar endianness.

   Constantes FNV-1a de 32 bits: offset basis `0x811c9dc5`, primo
   `0x01000193`. Por cada byte: `hash ^= byte; hash *= primo;`.

Esto responde directamente a *"detectará (...) contenido alterado"* del
enunciado: sin checksum, el verificador solo puede notar mensajes de más o
de menos, no un mensaje con bytes cambiados.

**Tamaño final del payload: 32 bytes** (`8 + 4 + 4 + 16`, sin relleno del
compilador gracias al orden de los campos).

### Por qué FNV-1a y no otro método de checksum

El checksum existe para un escenario concreto: si un bug de concurrencia
nuestro (un `put`/`get` mal protegido, un índice calculado mal, una
lectura de memoria que no terminó de escribirse) deja un mensaje con
`fill` corrupto pero `producer_id`/`sequence` intactos, el chequeo cruzado
del punto 1 (comparar sobre contra payload) no lo detecta — ambos siguen
coincidiendo. Hace falta algo que dependa de *todo* el contenido del
payload, incluido `fill`.

Se compararon cuatro opciones:

| Método | Por qué se descartó / se eligió |
|---|---|
| **Suma o XOR de bytes** | Demasiado débil incluso para nuestro caso: ambas operaciones son conmutativas, así que si un bug intercambia dos bytes de posición (plausible en un error de indexación), el checksum da exactamente igual y la corrupción pasa invisible. No sirve para el propósito. |
| **CRC32** | Diseñado específicamente para canales de transmisión ruidosos: garantiza matemáticamente detectar todo error de un solo bit y ráfagas de error hasta cierto largo. Esas garantías responden a un modelo de falla que no es el nuestro — no hay cable ni radio entre productor y consumidor, solo memoria compartida en la misma máquina. Lo que puede fallar ahí es un bug de software (un `memcpy` a la casilla equivocada, una condición de carrera), que típicamente sobrescribe bytes enteros con otro contenido completamente distinto, no voltea un bit aislado. Para ese tipo de corrupción, las garantías extra de CRC32 no aportan nada sobre un hash genérico, y su implementación (tabla de 256 entradas) es más código para el mismo resultado práctico. |
| **FNV-1a** *(elegido)* | Hash genérico de propósito general: cualquier cambio en los 28 bytes de entrada cambia el resultado con probabilidad ~(1 − 1/2³²) ≈ 99.99999998%, que es la misma fuerza de detección que ofrece CRC32 para corrupción arbitraria (no de bit único). Algoritmo público, documentado, sin ambigüedad de versión, ~5 líneas sin tabla ni dependencias. |
| **XXH3** | Ver comparación completa abajo — mejor en teoría, pero sus ventajas no aplican a 28 bytes y trae una dependencia externa que requiere aprobación previa del profesor. |

En otras palabras: no necesitamos las garantías específicas de CRC32
contra errores de transmisión porque no hay transmisión — hay bugs de
software, y contra esos, la probabilidad de colisión de un hash de 32
bits ya es suficiente, sea cual sea el algoritmo.

### Alternativa descartada: XXH3 en vez de FNV-1a

Se consideró XXH3 (familia xxHash) por su mejor distribución y velocidad en
datos grandes. Se descartó porque:

- El enunciado permite "C estándar y POSIX; otra dependencia requiere
  aprobación previa" — XXH3 es una librería externa y el proyecto pide
  documentar procedencia y licencia de material reutilizado en la entrega.
  Para una pieza que es un detalle de verificación, no vale el costo de
  pedir esa aprobación con 3 semanas de plazo.
- Su ventaja de velocidad aparece procesando megabytes; sobre 28 bytes de
  entrada la diferencia frente a FNV-1a es, en la práctica, cero.
- Reimplementarla a mano para evitar la dependencia tampoco se justifica:
  la referencia real usa tablas de constantes y rutas distintas por rango
  de tamaño de entrada, es fácil de implementar "casi bien" pero no
  bit-compatible, y perdería la ventaja de usar la implementación afinada
  original.

FNV-1a se escribe en ~5 líneas, sin dependencias, y es trivial de explicar
y trazar a mano.

---

## D2 — Buffer circular puro (`include/buffer.h`)

- **Fecha:** 2026-10-14
- **Issue:** [#2](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/2) (Diseño A), slice 2
- **Quién decide:** Emmanuel · **Revisa:** Isaac

### Contexto

El buffer en sí todavía no necesita sincronización — eso es el slice 3
(D3). Esta entrada solo fija cómo se mueven `head`/`tail`/`count` sobre
`messages[]`, el arreglo de tamaño fijo definido en D1.

### Ajuste de alcance respecto al plan original

`buffer.h` necesita los `struct` de D1 para poder declarar sus funciones,
así que se adelantó la creación de `include/memory.h` (exactamente las
structs de D1, sin lógica nueva) a este slice. El slice 3 queda entonces
solo con `include/sync.h` y el pseudocódigo de `put`/`get`.

### Decisión

```c
bool buffer_is_full(const struct instance *inst);
bool buffer_is_empty(const struct instance *inst);
void buffer_push(struct instance *inst, const struct message *msg);
void buffer_pop(struct instance *inst, struct message *out);
```

- **Avance de índices:** `tail = (tail + 1) % capacity` al insertar,
  `head = (head + 1) % capacity` al extraer. Se usa `%` normal, no una
  máscara de bits (`& (capacity - 1)`), porque `capacity` acepta cualquier
  valor entre 1 y 1024 — no se exige que sea potencia de 2 — y el costo de
  una división entera de más no compite ni de cerca con el costo real del
  sistema (adquirir el mutex, despertar a un proceso dormido).
- **No son thread-safe por sí mismas.** No toman el mutex ni verifican
  nada más allá de lo que dice su precondición documentada. Quien las
  llama (la capa de sincronización, D3) ya debe tener el mutex tomado y
  ya debe haber confirmado `!buffer_is_full`/`!buffer_is_empty` — típicamente
  dentro del ciclo `while` de un `pthread_cond_wait`.
- **No asignan `enqueue_seq`, `dequeue_seq` ni `enqueue_time_ns`.**
  `buffer_push` recibe un `struct message` ya completo y solo lo copia a
  la casilla; esos tres campos los llena quien llama, porque asignarlos
  correctamente (sin saltos ni duplicados) exige estar bajo el mutex en el
  momento exacto de la operación — mezclarlo dentro del buffer forzaría a
  este módulo a conocer de sincronización, justo la separación que D1 (y
  el enunciado) pide mantener explícita.

---

## D3 — Protocolo de sincronización: `put` / `get` (`include/sync.h`)

- **Fecha:** 2026-10-15
- **Issue:** [#2](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/2) (Diseño A), slice 3
- **Quién decide:** Emmanuel · **Revisa:** Isaac

### Contexto

Es la capa que traduce a código las tres exigencias centrales del
enunciado sobre sincronización: separar exclusión de disponibilidad de
datos y de espacio; bloquear en vez de sondear; y no retener el bloqueo
que necesita quien puede satisfacer la espera.

`sync.h` es el **único** módulo que toca el mutex y las condvars.
`buffer.h` (D2) asume que ya están tomados; los módulos de participantes
(#12) y finisher (#16/#17) construyen sus operaciones protegidas sobre
`sync_lock`/`sync_unlock`.

### Pseudocódigo

```c
enum put_result sync_put(struct instance *inst, struct message *msg,
                         struct op_snapshot *snap)
{
    lock(&inst->mutex);

    while (buffer_is_full(inst) && inst->state == STATE_OPEN)
        cond_wait(&inst->not_full, &inst->mutex);

    if (inst->state != STATE_OPEN) {          /* el corte le gana al espacio */
        unlock(&inst->mutex);
        return PUT_REJECTED_CLOSING;           /* NO cuenta como aceptado */
    }

    msg->enqueue_seq     = ++inst->enqueue_seq;   /* primer mensaje -> 1 */
    msg->enqueue_time_ns = now_monotonic_ns();
    buffer_push(inst, msg);                       /* <-- toma efecto aqui */

    if (snap) {
        snap->seq       = msg->enqueue_seq;
        snap->time_ns   = msg->enqueue_time_ns;
        snap->occupancy = inst->count;
    }

    cond_signal(&inst->not_empty);
    unlock(&inst->mutex);
    return PUT_OK;
}

enum get_result sync_get(struct instance *inst, struct message *out,
                         struct op_snapshot *snap)
{
    lock(&inst->mutex);

    while (buffer_is_empty(inst) && inst->state == STATE_OPEN)
        cond_wait(&inst->not_empty, &inst->mutex);

    if (buffer_is_empty(inst)) {              /* implica state != OPEN */
        unlock(&inst->mutex);
        return GET_DRAINED;
    }

    uint64_t dseq = ++inst->dequeue_seq;
    buffer_pop(inst, out);                     /* <-- toma efecto aqui */

    if (snap) {
        snap->seq       = dseq;
        snap->time_ns   = now_monotonic_ns();
        snap->occupancy = inst->count;
    }

    cond_signal(&inst->not_full);
    unlock(&inst->mutex);
    return GET_OK;
}
```

### Predicados de espera, y por qué son asimétricos

| | Duerme mientras | Al salir del ciclo verifica |
|---|---|---|
| Productor | `count == capacity && state == OPEN` | `state != OPEN` → rechazado |
| Consumidor | `count == 0 && state == OPEN` | `count == 0` → drenado |

El productor sale apenas el estado deja de ser OPEN, aunque haya espacio
libre: *"una inserción concurrente queda aceptada antes de ese punto o
rechazada después"*. El chequeo de estado le gana al de espacio.

El consumidor hace lo contrario: sigue extrayendo en CLOSING mientras
queden mensajes, porque *"los consumidores permanecen hasta extraer todos
los mensajes aceptados"*.

Detalle fino: el consumidor verifica **solo** `count == 0`, sin mirar el
estado, y eso alcanza — si salió del ciclo con el buffer vacío, la única
forma posible es que el estado ya no sea OPEN (si fuera OPEN y vacío,
seguiría dormido). O sea, `count == 0` después del ciclo *implica* "vacío
y cerrándose".

### `signal` en operación normal, `broadcast` en el cierre

Regla: `signal` cuando el evento satisface a **un** esperador;
`broadcast` cuando cambia una condición **compartida por todos**.

Un `push` produce exactamente un mensaje (satisface a un consumidor); un
`pop` libera exactamente una casilla (satisface a un productor). Pasar a
CLOSING, en cambio, vuelve falso el predicado de todos a la vez — de ahí
`sync_broadcast_all`.

`signal` es seguro acá porque las condvars están separadas por rol: en
`not_empty` solo duermen consumidores, todos con el mismo predicado, y en
`not_full` solo productores. **Si se unificaran en una sola condvar,
`signal` pasaría a ser un bug silencioso** (podría despertar a un
productor cuando hacía falta un consumidor, dejando trabajo sin hacer).
Esta precondición hay que sostenerla si el código cambia.

Costo que evita, con los números del caso de estrés (32 consumidores,
1000 mensajes): `broadcast` en cada push daría 32 000 despertares, de los
cuales 31 000 son inútiles — cada uno con su cambio de contexto,
adquisición de mutex y reevaluación del predicado. Con `signal` son 1000.

Efecto secundario aceptado: un consumidor recién llegado puede "ganarle"
el mensaje al que fue despertado, que se vuelve a dormir. No se pierde
nada; el enunciado no exige equidad (*"No se exige reparto equitativo
entre consumidores ni espera acotada"*).

`all_left` tiene un solo esperador posible (el finisher, y solo puede
haber uno), así que también le basta `signal`.

### Capturar bajo protección, escribir después

`struct op_snapshot` existe para cumplir *"capturar bajo protección los
datos necesarios para registrar un evento y escribirlos después"*. La
ocupación y la marca de tiempo se leen dentro de la región crítica; el
`fprintf` al CSV ocurre afuera, ya sin el mutex. Que la API lo devuelva
explícitamente hace difícil equivocarse: no hace falta volver a leer
`inst->count` después de soltar el bloqueo (que daría un valor distinto).

`enqueue_time_ns` también se toma bajo el mutex, a propósito: así el
orden de las marcas de tiempo coincide con el orden de `enqueue_seq`. Si
se tomara afuera, dos mensajes podrían quedar con marcas invertidas
respecto a su orden real de inserción.

### Reloj de las condvars

`sync_init_primitives` configura las condvars con
`pthread_condattr_setclock(CLOCK_MONOTONIC)`, no el reloj por defecto
(`CLOCK_REALTIME`). Hace falta para la pausa interrumpible del Issue #13,
que usa `pthread_cond_timedwait`: con el reloj por defecto, un cambio de
hora del sistema alteraría la duración de las esperas.

### Ausencia de deadlock

Un solo mutex, siempre tomado y soltado en la misma función, sin
anidamiento: no hay dos bloqueos que puedan adquirirse en orden distinto,
que es la condición necesaria para un deadlock por espera circular.
`cond_wait` suelta el mutex atómicamente al dormir, así que nadie espera
por espacio o datos reteniendo el bloqueo que el otro necesita — la regla
explícita del enunciado.

---

## D4 — Estructura del repositorio y ubicación de los ejecutables

- **Fecha:** 2026-10-15
- **Issue:** [#1](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/1)
- **Quién decide:** Isaac · **Revisa:** Emmanuel
- **Contexto:** el enunciado sugiere `src/`, `include/`, `tests/`, `scripts/`, `results/` y
  `docs/`, y muestra los ejecutables invocados como `./creator`, `./producer`, etc.
- **Decisión:** se usan esas carpetas. Cada ejecutable tiene su `main` en `src/<nombre>.c`;
  el resto de `src/*.c` son módulos comunes que se enlazan en los cuatro ejecutables. Los
  ejecutables se generan en la raíz y los objetos en `build/`.
- **Alternativas descartadas:** generar los ejecutables en `bin/`, porque obliga a cambiar
  los comandos de ejemplo del enunciado.

---

## D5 — Flags de compilación y macros de características

- **Fecha:** 2026-10-15
- **Issue:** [#1](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/1)
- **Quién decide:** Isaac · **Revisa:** Emmanuel
- **Contexto:** el enunciado exige `-std=c17 -Wall -Wextra -Wpedantic -Werror -pthread` y
  pide añadir las macros de características y bibliotecas necesarias. Con `-std=c17` estricto,
  glibc oculta las declaraciones POSIX si no se define una macro de características.
- **Decisión:** `_POSIX_C_SOURCE=200809L` en `CPPFLAGS`; se enlaza con `-lm` y, en Linux,
  con `-lrt`.
- **Alternativas descartadas:** `-std=gnu17` o `_GNU_SOURCE`, porque relajan el estándar
  exigido y pueden ocultar dependencias no portables.

---

## D6 — Makefile incremental

- **Fecha:** 2026-10-15
- **Issue:** [#1](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/1)
- **Quién decide:** Isaac · **Revisa:** Emmanuel
- **Contexto:** los módulos se implementan en issues distintos a lo largo del proyecto.
- **Decisión:** el Makefile descubre los módulos con `wildcard` y solo construye los
  ejecutables cuyo `main` ya existe, así `make` y `make test` funcionan en cada etapa sin
  editar listas de archivos. Genera dependencias de headers con `-MMD -MP`. `test` es un
  placeholder hasta los issues de pruebas (#19–#22).
- **Alternativas descartadas:** listar archivos a mano, porque genera conflictos de merge
  entre PRs paralelos.

---

## D7 — Resultados versionados

- **Fecha:** 2026-10-15
- **Issue:** [#1](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/1)
- **Quién decide:** Isaac · **Revisa:** Emmanuel
- **Contexto:** el enunciado pide excluir resultados masivos pero conservar los necesarios
  para reproducir el informe.
- **Decisión:** `.gitignore` excluye `results/*` salvo `results/seleccionados/`, donde se
  guardan los CSV, semillas y gráficas citados en el informe.

---

## D8 — Registro de participantes (`include/participants.h`)

- **Fecha:** 2026-10-15
- **Issue:** [#3](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/3) (Diseño B)
- **Quién decide:** Isaac · **Revisa:** Emmanuel

### Contexto

- *"Los identificadores de productor y consumidor serán únicos dentro de su
  respectivo rol durante toda la vida de la instancia."* → no basta con
  rechazar un id que está activo: un id que ya se retiró tampoco puede
  volver a usarse.
- *"El sistema soportará al menos 32 productores y 32 consumidores por
  instancia."*
- *"Los cambios de estado y la admisión de participantes se coordinarán con
  las operaciones del buffer."* → el registro usa el mismo mutex que
  `put`/`get` (D3).
- El resumen pide *"tiempo acumulado de espera (...) por proceso"*, y el
  finisher lo emite cuando esos procesos ya terminaron.

### Decisión

Una tabla fija por rol dentro de `struct instance`, de
`MAX_PARTICIPANTS_PER_ROLE = 64` casillas. Cada registro exitoso ocupa una
casilla **para siempre**: al retirarse, la casilla pasa a `SLOT_LEFT` pero
conserva el id, así el chequeo de duplicados cubre toda la vida de la
instancia sin estructuras extra.

```c
struct participant_slot {   /* 64 bytes */
    uint32_t id, state;      /* enum slot_state: FREE / ACTIVE / LEFT */
    int32_t  pid; uint32_t reserved;
    uint64_t joined_ns, left_ns;
    uint64_t ops, wait_ns, wait_count, lock_ns;   /* se copian al retirarse */
};
struct participant_table { uint32_t used, reserved; struct participant_slot slots[64]; };
```

`participant_register`, todo bajo el mutex y en este orden:

1. `state != OPEN` → `REG_NOT_OPEN`.
2. Recorrer las `used` casillas del rol; si alguna tiene el mismo `id` →
   `REG_DUPLICATE_ID`.
3. `used == 64` → `REG_TABLE_FULL`.
4. Ocupar `slots[used++]` como `SLOT_ACTIVE`, incrementar
   `active_producers`/`active_consumers` y, si `metrics.first_start_ns == 0`,
   fijarlo (inicio de operación del primer trabajador, para throughput).

Los rechazos no tocan la tabla: es la evidencia que pide #12 (*"rechazo sin
corromper el registro existente"*).

`participant_leave`, bajo el mutex: casilla a `SLOT_LEFT`, copia de
`struct participant_stats` a la casilla, `left_ns`, y decremento del
contador de activos. Los contadores `active_*` (D1) se mantienen aparte de
la tabla para que la precondición del cierre (D10) sea O(1).

### Alternativas descartadas

- **Bitmap de ids usados (p. ej. ids 1..1024).** Garantiza unicidad de por
  vida, pero obliga a limitar el rango de ids y no tiene dónde guardar las
  métricas por proceso para el resumen.
- **Casillas reutilizables al retirarse.** Rompe la unicidad de por vida o
  exige una segunda estructura de ids históricos.
- **64 y no 32.** 32 es el mínimo exigido; 64 deja margen para que el
  profesor agregue participantes en la demostración. Son 4 KB por rol.

**Límite documentado:** una instancia acepta como máximo 64 registros por
rol en toda su vida; el 65.º se rechaza con `REG_TABLE_FULL` y un mensaje
claro. Los ids válidos son cualquier `uint32_t` mayor que 0 (lo valida la
CLI, #4).

---

## D9 — Conexiones en curso: puerta de conexión con contador de referencias

- **Fecha:** 2026-10-15
- **Issue:** [#3](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/3) (Diseño B)
- **Quién decide:** Isaac · **Revisa:** Emmanuel

### Contexto

*"El finalizador espera confirmaciones de retirada y garantiza que no
quedan usuarios ni esperas sobre las primitivas antes de destruirlas.
También debe resolver con seguridad las conexiones que estaban en curso al
comenzar el cierre."*

El problema concreto: un proceso hace `shm_open` + `mmap` y valida `magic`
(D1), pero todavía no tomó el mutex para registrarse. Si en ese instante
el finisher ve `active_* == 0` y destruye el mutex, ese proceso hace
`pthread_mutex_lock` sobre un mutex destruido: comportamiento indefinido.
Los contadores `active_*` no lo ven porque todavía no se registró.

### Decisión

Un contador atómico de conexiones con un bit de "puerta cerrada":

```c
_Atomic uint32_t refs;      /* bit 31: REFS_GATE_CLOSED; bits 0..30: conexiones */
```

**Conectar** (`instance_attach`), sin mutex:

```c
mapear; validar magic (acquire) y version     /* D1 */
v = load(refs)
do {
    if (v & REFS_GATE_CLOSED) { munmap; return ATTACH_CLOSING; }  /* no toca primitivas */
} while (!compare_exchange(&refs, &v, v + 1));
return ATTACH_OK;           /* desde aquí, y solo desde aquí, puede usar el mutex */
```

**Desconectar** (`instance_detach`), bajo el mutex:
`fetch_sub(&refs, 1)`; `signal(all_left)`; `unlock`; `munmap`.

**Cerrar la puerta:** el finisher hace `fetch_or(&refs, REFS_GATE_CLOSED)`
dentro de la misma región crítica de `OPEN → CLOSING` (D10).

**Esperar:** el finisher, bajo el mutex,
`while ((refs & REFS_COUNT_MASK) > 1) wait(all_left)`. Es `> 1` porque el
propio finisher está conectado.

### Por qué es correcto

- **Nadie entra después de cerrar la puerta.** El CAS de conexión falla en
  cuanto observa el bit, y el bit nunca se apaga. Las conexiones que el
  finisher espera forman un conjunto fijo que solo puede decrecer.
- **Ninguna espera perdida.** Los incrementos ocurren solo con la puerta
  abierta, o sea antes de que el finisher empiece a esperar. Todo
  decremento posterior ocurre bajo el mutex seguido de `signal(all_left)`,
  así que el finisher no puede dormirse sobre un predicado que ya cambió.
- **Quien falla la conexión no toca primitivas.** Un proceso que ve la
  puerta cerrada solo desmapea su propia vista. Aunque el finisher ya haya
  destruido el mutex y hecho `shm_unlink`, el mapeo de ese proceso sigue
  siendo válido hasta su `munmap` (el objeto vive hasta el último mapeo).
- **Toda conexión exitosa se resuelve.** Un proceso conectado que llega
  tarde a registrarse recibe `REG_NOT_OPEN` (D8), se desconecta y con eso
  libera al finisher. Un participante registrado se desconecta al
  retirarse.

### Alternativas descartadas

- **Esperar solo `active_* == 0`.** No ve las conexiones en curso; es
  justamente el caso que pide el enunciado.
- **Contador de conexiones sin bit de puerta.** Deja abierta la carrera
  "el finisher lee 0 y destruye; un proceso incrementa justo después y
  usa el mutex".
- **Hacer `shm_unlink` al pasar a CLOSING.** Ayuda (nadie nuevo puede
  abrir el nombre) pero no resuelve a quien ya lo abrió. Se mantiene al
  final del cierre, después de destruir las primitivas.

---

## D10 — Máquina de estados OPEN / CLOSING / CLOSED y protocolo de cierre

- **Fecha:** 2026-10-15
- **Issue:** [#3](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/3) (Diseño B)
- **Quién decide:** Isaac · **Revisa:** Emmanuel

### Transiciones

| De | A | Quién | Dónde | Protección |
|---|---|---|---|---|
| (no existe) | OPEN | `creator` | pasos 7–8 de D1 | publicación con `magic` (release/acquire) |
| OPEN | CLOSING | `finisher` | `closing_begin` | mutex; misma región crítica que la precondición y el cierre de la puerta |
| CLOSING | CLOSED | `finisher` | `closing_release` | mutex tomado y `refs == 1` (solo el finisher) |
| CLOSED | (no existe) | `finisher` | `closing_release` | `shm_unlink` tras destruir las primitivas |

No hay transiciones de regreso. Un `closing_begin` rechazado no cambia
nada: la instancia sigue OPEN.

`state` solo se escribe con el mutex tomado y solo lo escribe el finisher
(además del `creator` antes de publicar). Se lee siempre bajo el mutex.

### Precondición y finalizador único

```c
enum close_result closing_begin(struct instance *inst)
{
    lock(&inst->mutex);
    if (inst->state != STATE_OPEN) {             /* otro finisher ya cortó */
        unlock(&inst->mutex);
        return CLOSE_ALREADY_CLOSING;
    }
    if (inst->active_consumers == 0 &&
        (inst->active_producers > 0 || inst->count > 0)) {
        unlock(&inst->mutex);                    /* sigue OPEN, sin tocar nada */
        return CLOSE_NO_CONSUMER;
    }
    inst->state = STATE_CLOSING;                 /* <-- punto de corte */
    fetch_or(&inst->refs, REFS_GATE_CLOSED);     /* D9 */
    inst->metrics.closing_start_ns = now_monotonic_ns();
    sync_broadcast_all(inst);                    /* D3; incluye la condvar de la pausa (#11) */
    unlock(&inst->mutex);
    return CLOSE_OK;
}
```

- **Finalizador único sin bandera extra.** La transición misma es la
  "reserva": el primer finisher que la hace queda como conductor; cualquier
  otro ve `state != OPEN` y recibe `CLOSE_ALREADY_CLOSING`. Si llega
  después de cerrada la puerta, falla antes, en la conexión
  (`ATTACH_CLOSING`). En ambos casos el mensaje es "ya hay un cierre en
  curso".
- **Un finisher rechazado por la precondición no reserva nada**, así que el
  usuario puede iniciar un consumidor y volver a pedir el cierre.
- **Instancia sin trabajadores:** `active_producers == 0` y `count == 0`, la
  precondición se cumple y el cierre procede.

### Punto de corte

`state = STATE_CLOSING` ocurre bajo el mutex. `sync_put` (D3) revisa el
estado bajo el mismo mutex justo antes de `buffer_push`, así que cada
inserción queda totalmente antes del corte (aceptada, con `enqueue_seq`) o
totalmente después (`PUT_REJECTED_CLOSING`). El registro (D8) usa el mismo
chequeo, así que tampoco hay registros a medias. Al cortar se guardan
`enqueue_seq`, `dequeue_seq` y `count` en el log del finisher: ahí se
audita `aceptados = extraídos + pendientes`.

### Qué observa cada participante

| Participante | Al pasar a CLOSING |
|---|---|
| Productor bloqueado en `not_full` | `broadcast` lo despierta, ve `state != OPEN` → `PUT_REJECTED_CLOSING` → se retira. |
| Productor en pausa simulada | La pausa duerme en una condvar dedicada (`closing`, agregada en #11 por la revisión de #29), nunca en `not_full`. El `broadcast` la interrumpe y el productor sale sin intentar insertar (#13). |
| Consumidor con mensajes pendientes | Sigue extrayendo; `sync_get` solo devuelve `GET_DRAINED` con el buffer vacío. |
| Consumidor bloqueado en vacío | `broadcast` lo despierta, ve `count == 0` → `GET_DRAINED` → se retira. |
| Consumidor procesando un mensaje | Termina el procesamiento simulado (no se interrumpe) y vuelve a `sync_get`. |
| Proceso conectado sin registrar | `REG_NOT_OPEN` → se desconecta (D9). |
| Proceso que aún no se conectó | `ATTACH_CLOSING` → no toca primitivas. |

### Orden del cierre en el finisher

```text
instance_attach(name)                          # toma su propia referencia
r = closing_begin()                             # D10
si r != CLOSE_OK: log, instance_detach, salir con error (≠ 0)
closing_wait_all_left()                         # retorna con el mutex tomado
closing_release(name)                           # CLOSED, unlock, destroy, shm_unlink
close_ns = now - metrics.closing_start_ns       # "hasta completar la liberación"
summary_write_csv(summary, inst, close_ns)      # lee el segmento, aún mapeado
munmap(inst)
```

`closing_release` no desmapea para que el resumen pueda leer los
agregados sin copiarlos antes. La lectura sin mutex es segura porque ya no
queda nadie conectado ni primitivas que proteger.

### Ausencia de deadlock en el cierre

El finisher espera en `all_left` sobre el mismo y único mutex (D3), sin
anidar. Cada conexión que espera se resuelve en tiempo finito con
participantes cooperantes: los productores salen por el corte, los
consumidores salen al drenar (y hay al menos uno, por la precondición, si
quedaban productores o datos) y los procesos sin registrar salen por
`REG_NOT_OPEN`.

---

## D11 — Métricas y resumen del finisher (`include/metrics.h`)

- **Fecha:** 2026-10-15
- **Issue:** [#3](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/3) (Diseño B)
- **Quién decide:** Isaac · **Revisa:** Emmanuel

### Qué se mide, dónde y cuándo

| Métrica | Se toma en | Bajo mutex | Se guarda en |
|---|---|---|---|
| Aceptados | `enqueue_seq` (D3) | sí | segmento |
| Extraídos | `dequeue_seq` (D3) | sí | segmento |
| Pendientes | `count` | sí | segmento |
| Rechazados por el corte | `sync_put` | sí | `metrics.rejected_puts` |
| Latencia inserción → extracción | `sync_get`: `snap.time_ns - msg.enqueue_time_ns` | sí | `metrics.latency` (histograma) |
| Espera por espacio / datos | alrededor del `while … cond_wait` | se mide adentro | `op_snapshot.wait_ns` → stats locales → casilla |
| Adquisición de la exclusión | alrededor de `pthread_mutex_lock` | justo antes/después | `op_snapshot.lock_ns` → stats locales → casilla |
| Primer inicio de trabajador | registro del primer participante | sí | `metrics.first_start_ns` |
| Última extracción | `sync_get` | sí | `metrics.last_dequeue_ns` |
| Duración del cierre | `closing_start_ns` → fin de `closing_release` | — | local del finisher |

Todas las marcas usan `CLOCK_MONOTONIC`.

**Cambio propuesto a `sync.h` (D3):** `struct op_snapshot` gana `wait_ns`,
`wait_count` y `lock_ns` para que `sync_put`/`sync_get` devuelvan esas
duraciones sin que el llamador vuelva a tocar el mutex. Lo implementa #11.

**Cambio a `memory.h` (D1):** se reemplaza el comentario "tabla de ids
usados por rol: se agrega en #12" por `refs` (D9), `producers` y
`consumers` (D8) y `metrics` (esta entrada). El comentario de `all_left`
pasa a decir que el finisher espera `refs == 1`, no `activos == 0` (D9). `memory.h` incluye
`participants.h` y `metrics.h`; estos solo hacen *forward declaration* de
`struct instance`, así que no hay inclusión circular.

### Latencia: histograma log-lineal en el segmento

Guardar cada latencia exigiría memoria no acotada en el segmento. Se usa un
histograma de 496 cubetas (4 KB): valores menores que 8 ns son exactos; el
resto se divide en 8 sub-cubetas por potencia de 2:

```c
if (ns < 8) bucket = ns;
else { e = floor(log2(ns)); sub = (ns >> (e - 3)) & 7; bucket = (e - 2) * 8 + sub; }
```

- **Media:** exacta (`sum_ns / count`).
- **Percentil 95:** se recorre el histograma acumulando hasta el 95 % y se
  reporta el punto medio de la cubeta. Error relativo ≤ 6.25 %. **El
  resumen lo declara como aproximado.**
- **Para el experimento** el p95 exacto se calcula aparte desde los CSV de
  los consumidores (`t_ns - enqueue_time_ns` de cada `GET`, D12).

`lat_hist_add` corre dentro de la región crítica de `sync_get`: son unas
pocas operaciones enteras sin llamadas al sistema, un costo despreciable
frente a la adquisición del mutex. Hacerlo afuera obligaría a volver a
tomar el mutex.

### Esperas combinadas, declaradas como tales

- **Espera por datos del consumidor** incluye la espera final en vacío
  hasta que llega el `broadcast` del cierre. No es "falta de productores"
  pura: el resumen lo aclara en la columna `note`.
- **Espera por espacio del productor** no incluye su pausa simulada; la
  pausa no es una espera de sincronización.
- **Adquisición de la exclusión** mide solo `pthread_mutex_lock` en
  `put`/`get`, incluido el re-adquirir al despertar de `cond_wait`, porque
  POSIX no los separa. Se declara así.

### Throughput

`extraídos / (last_dequeue_ns - first_start_ns)`, en mensajes por segundo,
como define el enunciado.

### Formato del resumen (`--summary`)

CSV en formato largo, una métrica por fila, para que los scripts lo lean
sin conocer las columnas de antemano:

```text
metric,role,id,value,unit,note
```

| Columna | Tipo | Contenido |
|---|---|---|
| `metric` | texto | nombre de la métrica (tabla siguiente) |
| `role` | texto | `instance`, `producer` o `consumer` |
| `id` | entero | id del participante; `0` para `instance` |
| `value` | número | entero, o decimal con punto y hasta 6 decimales |
| `unit` | texto | `msgs`, `ns`, `msgs_per_s`, `count`, `bool` |
| `note` | texto | aclaración sin comas; vacío si no aplica |

Filas de instancia (`role = instance`, `id = 0`):

| `metric` | `unit` | `note` |
|---|---|---|
| `accepted` | msgs | |
| `extracted` | msgs | |
| `pending` | msgs | |
| `rejected_puts` | count | intentos rechazados por el corte; no son aceptados |
| `invariant_ok` | bool | 1 si `accepted = extracted + pending` y `pending = 0` |
| `latency_mean` | ns | exacta |
| `latency_p95` | ns | aproximada por histograma (error ≤ 6.25 %) |
| `latency_max` | ns | |
| `throughput` | msgs_per_s | primer inicio de trabajador a última extracción |
| `active_interval` | ns | denominador del throughput |
| `close_duration` | ns | de CLOSING a liberación completa |
| `producers` | count | registros de por vida |
| `consumers` | count | registros de por vida |

Filas por participante (una por casilla usada; `role` y `id` del
participante):

| `metric` | `unit` | `note` |
|---|---|---|
| `ops` | msgs | aceptados (productor) o extraídos (consumidor) |
| `wait_space` / `wait_data` | ns | productor / consumidor; para el consumidor: incluye la espera final hasta el cierre |
| `wait_count` | count | |
| `lock_acquire` | ns | incluye re-adquisición al despertar de `cond_wait` |
| `lifetime` | ns | `left_ns - joined_ns` |

---

## D12 — Formato de los CSV de productor, consumidor y finisher

- **Fecha:** 2026-10-15
- **Issue:** [#3](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/3) (Diseño B)
- **Quién decide:** Isaac · **Revisa:** Emmanuel

### Contexto

*"Los logs registrarán rol, id, evento, marca monotónica, identidad del
mensaje, enqueue_seq y ocupación capturada en la operación protegida. Los
retiros incluirán un dequeue_seq global (...) Se registrarán también
incorporaciones, retiros y cambios de estado suficientes para auditar el
cierre."*

### Decisión

Un CSV por proceso (`--log`), **las mismas columnas para todos los roles**,
así el verificador (#7) puede concatenarlos sin adaptar el parser. Cabecera
en la primera línea, separador `,`, fin de línea `\n`, sin comillas (ningún
campo contiene comas). Un campo que no aplica a un evento va vacío.

```text
role,id,event,t_ns,producer_id,sequence,enqueue_seq,dequeue_seq,enqueue_time_ns,occupancy,checksum,payload_ok,detail
```

| Columna | Tipo | Contenido |
|---|---|---|
| `role` | texto | `producer`, `consumer` o `finisher` |
| `id` | uint32 | id del proceso; `0` para el finisher |
| `event` | texto | ver tabla de eventos |
| `t_ns` | uint64 | `CLOCK_MONOTONIC` del evento; en `PUT`/`GET` es la marca tomada bajo el mutex |
| `producer_id` | uint32 | identidad del mensaje |
| `sequence` | uint64 | identidad del mensaje (por productor, desde 1) |
| `enqueue_seq` | uint64 | orden global de inserción |
| `dequeue_seq` | uint64 | orden global de extracción |
| `enqueue_time_ns` | uint64 | marca de inserción que viaja en el mensaje |
| `occupancy` | uint32 | `count` justo después de la operación, capturado bajo el mutex |
| `checksum` | hex | FNV-1a del payload (D1), 8 dígitos hex con prefijo `0x` |
| `payload_ok` | 0/1 | el consumidor recalculó `fill` y `checksum` y coinciden |
| `detail` | texto | motivo de un rechazo o dato de auditoría, sin comas |

### Eventos y campos que llevan

| Rol | `event` | Campos además de `role,id,event,t_ns` |
|---|---|---|
| prod./cons. | `JOIN` | `detail` = pid |
| prod./cons. | `JOIN_REJECTED` | `detail` = `duplicate_id` / `not_open` / `table_full` / `closing` / `not_found` |
| productor | `PUT` | `producer_id, sequence, enqueue_seq, enqueue_time_ns, occupancy, checksum` |
| productor | `PUT_REJECTED` | `producer_id, sequence` del intento; `detail` = `closing` |
| consumidor | `GET` | `producer_id, sequence, enqueue_seq, dequeue_seq, enqueue_time_ns, occupancy, checksum, payload_ok` |
| consumidor | `PROCESSED` | `producer_id, sequence, dequeue_seq` (fin del procesamiento simulado) |
| consumidor | `DRAINED` | `occupancy` (= 0) |
| prod./cons. | `LEAVE` | `detail` = `quota_done` / `closing` / `drained` |
| finisher | `CLOSE_REQUEST` | — |
| finisher | `CLOSE_REJECTED` | `detail` = `no_consumer` / `already_closing` |
| finisher | `CLOSING` | `enqueue_seq` (aceptados), `dequeue_seq` (extraídos) y `occupancy` (pendientes) **en el punto de corte** |
| finisher | `ALL_LEFT` | — |
| finisher | `CLOSED` | `detail` = `released` |

Ejemplo:

```text
role,id,event,t_ns,producer_id,sequence,enqueue_seq,dequeue_seq,enqueue_time_ns,occupancy,checksum,payload_ok,detail
producer,1,PUT,81234567000,1,1,1,,81234567000,1,0x9c3a51e2,,
consumer,1,GET,81234612000,1,1,1,1,81234567000,0,0x9c3a51e2,1,
```

### Lo que permite verificar

- **Pérdidas y duplicados:** cruzar `PUT` con `GET` por
  `(producer_id, sequence)`.
- **Contenido alterado:** `GET.payload_ok = 0`, o `GET.checksum ≠
  PUT.checksum` para la misma identidad.
- **FIFO:** ordenando los `GET` de todos los consumidores por
  `dequeue_seq`, la secuencia de `enqueue_seq` tiene que ser creciente y sin
  huecos. Esto no depende del orden en que se escribieron los archivos.
- **Corte y cierre:** ningún `PUT` con `enqueue_seq` mayor que el del
  evento `CLOSING`; al final, `aceptados = extraídos`.
- **Latencia exacta:** `GET.t_ns - GET.enqueue_time_ns`.

### Opción agregada al finisher

El finisher acepta `--log <ruta>` opcional (el enunciado permite agregar
opciones) para registrar sus cambios de estado. Sin `--log`, imprime los
mismos eventos en la consola.

---

## D13 — Módulo de memoria compartida y `creator`

- **Fecha:** 2026-10-17
- **Issue:** [#9](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/9)
- **Quién decide:** Isaac · **Revisa:** Emmanuel

### Contexto

D1 fija el layout, el orden de creación y qué deshacer ante cada falla. Esta
entrada agrega lo que D1 deja abierto al implementarlo, más las
observaciones de la revisión de #29 que el issue #9 recogió.

### API (`include/memory.h`, `src/memory.c`)

```c
size_t             instance_segment_size(uint32_t capacity);
bool               instance_name_is_valid(const char *name);
const char        *memory_result_str(enum memory_result r);
enum memory_result memory_create(const char *name, uint32_t capacity,
                                 struct instance **out, size_t *map_len);
enum memory_result memory_open(const char *name, struct instance **out,
                               size_t *map_len);
int                memory_unmap(struct instance *inst, size_t map_len);
int                memory_unlink(const char *name);
```

`memory_create` sigue al pie de la letra los 8 pasos y la tabla de
deshacer de D1. `memory_open` es la parte de "conectarse" que no toca
primitivas: `instance_attach` (D9, #12) la usa y después toma la
referencia de conexión.

Los resultados son un `enum memory_result` en vez de `-errno`, porque los
casos que el usuario tiene que distinguir (ya existe, no existe, no está
lista, incompatible) no se corresponden uno a uno con un `errno`. Para
`MEM_SYS_ERROR`, `errno` queda con la causa.

### Formato del nombre

`/` seguido de 1 a 30 caracteres de `[A-Za-z0-9_-]` (31 en total). POSIX
solo garantiza el `/` inicial y la ausencia de otros `/`; el resto depende
del sistema. 31 es el límite de macOS (`PSHMNAMLEN`) y cabe holgado en el
de Linux (`NAME_MAX`). Restringir los caracteres evita espacios, `/` y
caracteres de control en nombres que también se usan en mensajes y
scripts. El ejemplo del enunciado (`/soa_p2_g1`) cumple.

### Validaciones al abrir (revisión de #29)

`memory_open` rechaza el segmento, desmapeándolo, si:

1. Es más chico que `struct instance`: un `creator` que aún no hizo
   `ftruncate`, o un segmento ajeno. No se puede ni leer `magic` →
   `MEM_NOT_READY`.
2. `magic != INSTANCE_MAGIC` (con `acquire`) → `MEM_NOT_READY`.
3. `version`, `capacity` (1..1024) o el tamaño del segmento no coinciden
   → `MEM_INCOMPATIBLE`.

**Tamaño en macOS:** Linux reporta en `fstat` el tamaño exacto del
`ftruncate`; macOS lo redondea al tamaño de página. Se acepta el exacto o
ese redondeo, nada más. Las pruebas usan capacidades que dan tamaños
distintos incluso redondeados.

### Otros detalles de la implementación

- **`ftruncate` se reintenta ante `EINTR`**, como pide el enunciado sobre
  interrupciones de llamadas. Si falla por otra causa, se deshace como en
  la fila "3 o 4" de D1.
- **`_Static_assert(ATOMIC_INT_LOCK_FREE == 2)`** en `memory.h`: los
  atómicos del segmento (`magic`, `refs`) solo sincronizan entre procesos
  si son lock-free.
- **`struct message` gana `uint32_t reserved`** después de `producer_id`.
  Hace explícitos los 4 bytes de relleno que el compilador ya insertaba
  (`sizeof` sigue en 64, verificado con `_Static_assert`), en línea con el
  cuidado de D1 sobre `struct payload`. Siempre vale 0.
- **`sync_init_primitives` y `sync_destroy_primitives` se implementan en
  `src/sync.c` en este issue**, porque el `creator` las necesita en el
  paso 6. Inicializan las tres condvars actuales; #11 agrega `put`/`get`,
  `lock`/`unlock`, `broadcast` y la condvar dedicada de la pausa.
- **`creator` desmapea al terminar**: el objeto de memoria compartida
  persiste hasta el `shm_unlink` del finisher.

### `pthread_condattr_setclock` en macOS

macOS no implementa la opción *Clock Selection* de POSIX, así que
`pthread_condattr_setclock` no existe ahí. Se usa solo si
`_POSIX_CLOCK_SELECTION > 0`, que en GNU/Linux (el entorno de referencia)
siempre se cumple. En macOS las condvars quedan con el reloj por defecto y
sirven solo para desarrollar y probar localmente. Es una limitación del
entorno documentada también en #23.

### Análisis de argumentos de `creator`

Mínimo y estricto: `--name` y `--capacity` obligatorios, sin repetir, sin
opciones desconocidas. La capacidad se convierte con `strtoull` y se
rechaza si está vacía, tiene signo o espacios, basura al final u
overflow. El issue #4 unifica este análisis con el de los demás
ejecutables.

---

## D14 — Compilador por defecto en el Makefile

- **Fecha:** 2026-10-17
- **Issue:** [#9](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/9) (corrige D5)
- **Quién decide:** Isaac · **Revisa:** Emmanuel

### Contexto

D5 puso `CC ?= gcc`. Al revisar #33 se vio que esa línea nunca tiene
efecto: make predefine `CC = cc`, y `?=` solo asigna si la variable no
tiene valor. Los comandos mostraban `cc`. En Linux `cc` suele ser `gcc`,
pero el enunciado pide GCC explícitamente.

### Decisión

```make
ifeq ($(origin CC),default)
CC := gcc
endif
```

`$(origin CC)` vale `default` solo si el valor viene de make. Así se usa
`gcc` por defecto y se respeta un compilador elegido por el usuario
(`make CC=clang` o variable de entorno), que era la intención de D5.

---

## D15 — Análisis y validación de argumentos (`include/cli.h`)

- **Fecha:** 2026-10-19
- **Issue:** [#4](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/4)
- **Quién decide:** Isaac · **Revisa:** Emmanuel

### Contexto

*"Se rechazarán nombres mal formados, identificadores duplicados, valores
fuera de rango y errores de conversión u overflow."* Los cuatro ejecutables
comparten opciones (`--name`, `--log`, …), así que las reglas tienen que
ser las mismas en todos.

### Decisión

Un módulo `cli` con una tabla que dice, por opción, qué roles la aceptan y
para cuáles es obligatoria:

| Opción | creator | producer | consumer | finisher | Valor |
|---|---|---|---|---|---|
| `--name` | obligatoria | obligatoria | obligatoria | obligatoria | formato de D13 |
| `--capacity` | obligatoria | — | — | — | entero 1..1024 |
| `--id` | — | obligatoria | obligatoria | — | entero 1..2³²−1 |
| `--mean-ms` | — | obligatoria | obligatoria | — | decimal en [0, 1000] |
| `--seed` | — | obligatoria | obligatoria | — | entero 1..2³²−1 |
| `--messages` | — | opcional | — | — | entero 1..2⁶⁴−1 |
| `--log` | — | obligatoria | obligatoria | opcional (D12) | ruta no vacía |
| `--summary` | — | — | — | obligatoria | ruta no vacía |

`cli_parse(rol, argc, argv, &opciones, err, len)` devuelve `CLI_OK`,
`CLI_HELP` o `CLI_ERROR` con el motivo en `err`. El ejecutable imprime
`<rol>: <motivo>` y la línea de uso en `stderr` y sale con 1.

- **`--mean-ms`, `--seed` y `--log` son obligatorias** para producer y
  consumer: la media define el escenario, la semilla hace reproducible la
  corrida (el experimento pide conservarlas) y cada trabajador tiene que
  escribir su CSV. `--messages` es la única opcional: sin ella el
  productor sigue hasta el cierre, como dice el enunciado.
- **Enteros: solo dígitos decimales.** `strtoull` aceptaría espacios
  iniciales, `+` y `-` (convierte `-5` en un número enorme), así que se
  exige que todo el texto sean dígitos antes de convertir y después se
  revisan `ERANGE`, el rango del tipo y el rango de la opción. Los ceros a
  la izquierda se leen en decimal (`007` = 7).
- **`--seed 0` se rechaza:** 0 es el único punto fijo de xorshift32 (#5).
  `--id 0` también, porque D8 usa ids mayores que 0.
- **`--mean-ms`: dígitos con parte decimal opcional** (`5`, `2.5`). Se
  valida el formato antes de `strtod`, que aceptaría `inf`, `nan`, `1e3`,
  `0x10`, signo y espacios. Después se exige un valor finito en [0, 1000].
- **Un valor que empieza con `--` se toma como valor faltante:** en
  `--log --id 3`, falta el valor de `--log`. Ninguna opción legítima
  empieza con `--`, y los números negativos empiezan con un solo `-`.
- **Opción desconocida, repetida o que no aplica al rol:** error. Ningún
  ejecutable ignora argumentos.
- **El módulo no abre archivos ni toca la instancia:** un rechazo nunca
  deja recursos a medias. Abrir el CSV es del logger (#6); si la ruta no se
  puede abrir, falla ahí.

El id duplicado y la instancia inexistente, que también son casos de
"Validación", no son de la CLI: se detectan al conectarse
(`participant_register`, `memory_open`) y se prueban en #19.

### Alternativas descartadas

- **`getopt_long`:** es una extensión GNU, no POSIX, y no evita escribir
  los conversores estrictos. Con ocho opciones, la tabla propia es más
  corta y más fácil de probar.
- **Valores por defecto para `--mean-ms` y `--seed`:** harían que una
  corrida olvide su semilla sin que nadie lo note.

---

## D16 — Verificador de los CSV (`include/verify.h`)

- **Fecha:** 2026-10-18
- **Issue:** [#7](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/7)
- **Quién decide:** Emmanuel · **Revisa:** Isaac

### Contexto

D12 fija el formato de los CSV y enumera, al final, lo que ese formato
*permite* verificar. Esta entrada define la herramienta que efectivamente
lo verifica, que es el issue #7.

El problema de fondo: con 32 productores y 32 consumidores (el caso de
estrés de #20), una condición de carrera puede perder, duplicar o
desordenar un mensaje entre miles sin que nada se caiga ni se note en la
salida por pantalla. Las pruebas de integración de #20 necesitan un
oráculo que decida "esta corrida fue correcta" de forma automática; sin
él solo podrían afirmar "no crasheó". Por eso #7 bloquea a #20.

### Forma de la herramienta

Un ejecutable `verify` que recibe N rutas de CSV de cualquier rol y los
cruza como un solo conjunto de eventos:

```text
./verify resultados/prod1.csv resultados/cons1.csv resultados/fin.csv
```

Sale con 0 si la corrida es correcta y distinto de 0 si encuentra una
violación, indicando cuál. Reporta **todas** las violaciones que encuentra,
no solo la primera: mismo criterio que `CHECK()` en `tests/test_util.h`, y
por la misma razón — al depurar una corrida fallida interesa el panorama
completo, no el primer síntoma.

El código se parte en dos para poder probarlo sin lanzar procesos:

- `src/verify_core.c` — parseo y chequeos, sin escribir a consola.
- `src/verify.c` — `main` delgado: abre los archivos, llama al núcleo,
  imprime el reporte y traduce el resultado a código de salida.

Así `tests/test_verify.c` ejercita la lógica con CSV en memoria
(`fmemopen`), sin tocar disco, igual que el resto de la suite.

### Política de parseo: rechazar antes que aceptar a medias

Un CSV ya escrito es una frontera del sistema: puede venir de un proceso
que murió a la mitad, de un disco lleno o de una edición a mano. Como la
herramienta existe justamente para detectar datos alterados, el parseo es
estricto y rechaza la fila completa ante cualquier duda, en vez de
quedarse con una versión parcial. Se rechaza:

- Un número de columnas distinto de 13 — incluida una coma dentro de
  `detail`, el único campo de texto libre. D12 garantiza que ningún campo
  lleva comas; si ese invariante se rompiera, la fila se rechaza en lugar
  de desalinear las columnas siguientes.
- Números con signo o con basura alrededor. `strtoull("-1")` devuelve
  `ULONG_MAX` **sin** marcar `ERANGE`, así que el signo se corta antes de
  convertir. Ninguna columna de D12 es negativa.
- Valores que no entran en el tipo de su columna (`uint32_t`) o que
  desbordan `uint64_t` (`ERANGE`).
- Un `detail` de 48 caracteres o más (`LOG_DETAIL_MAX`). Truncarlo sería
  alterar el dato en silencio; el valor más largo que define D12
  (`already_closing`) mide 15.
- Una línea que no entra en `LOG_LINE_MAX` (512 bytes, contra ~100 de una
  fila real). Este caso es el más traicionero: `fgets` entrega el resto en
  la llamada siguiente, y si los dos pedazos parsean, el verificador
  cuenta una fila que nunca se escribió. Se reprodujo con un archivo de
  una sola línea de datos que producía dos filas, la segunda inventada.
- Un error de lectura a mitad de archivo (`ferror`), que de otro modo se
  confunde con fin de archivo y daría por bueno un log incompleto.

Un campo que D12 marca como "no aplica a este evento" llega vacío y queda
en `0` (numéricos) o `""` (`detail`). No hay valores centinela: decidir
qué columna importa para cada `event` es trabajo de los chequeos, no del
parseo. Esto mantiene el parser simple y evita inventar un "ausente" que
chocaría con valores legítimos (`occupancy` vale 0 en un `DRAINED`).

### Los cuatro chequeos

1. **Conservación de identidades.** Cada `PUT` con `enqueue_seq` asignado
   tiene exactamente un `GET` con la misma identidad `(producer_id,
   sequence)`. Cero extracciones es una pérdida; dos o más, un duplicado.
   Al implementarlo (#7, slice 2) se agregaron dos casos que el issue no
   nombraba: un `GET` sin `PUT` (mensaje fantasma) y dos `PUT` con la
   misma identidad, que viola la unicidad que exige el enunciado y que de
   otro modo pasaría inadvertida cuando también hay dos `GET`, porque los
   conteos se cancelan. `PUT_REJECTED` se ignora: lleva identidad, pero
   ese mensaje nunca entró al buffer y contarlo convertiría cada rechazo
   legítimo del cierre en una pérdida.
2. **Integridad.** En cada par emparejado, `PUT.checksum ==
   GET.checksum` y `GET.payload_ok == 1`. Implementado así en #7, slice 3;
   podría ser más estricto recalculando el checksum, ver la limitación
   anotada abajo. Un `GET` puede producir las dos violaciones a la vez: el
   veredicto del consumidor sobre el mensaje que recibió y la comparación
   contra lo que registró el productor son hechos distintos.
3. **FIFO.** Para cada `GET`, `enqueue_seq == dequeue_seq`. Es la forma
   más fuerte y más simple de lo que D12 enuncia como "ordenar los `GET`
   por `dequeue_seq` y ver que `enqueue_seq` crece sin huecos": como hay
   una sola cola, el mensaje extraído k-ésimo tiene que ser exactamente el
   insertado k-ésimo.
4. **Corte y cierre.** Ningún `PUT` con `enqueue_seq` mayor al que reporta
   el evento `CLOSING`, y al final aceptados == extraídos. Al
   implementarlo (#7, slice 4) se agregó un tercer contraste: que la foto
   del `CLOSING` cierre sola, `aceptados == extraídos + pendientes`, ya
   que lo aceptado está extraído o todavía en el buffer y no hay un tercer
   lugar donde pueda estar. Un `PUT` con `enqueue_seq` *igual* al corte es
   legítimo — es el último aceptado —, así que la comparación es estricta
   solo por encima. Sin evento `CLOSING` el chequeo no corre y lo informa,
   en vez de dar el cierre por bueno: verificar los CSV de una corrida a
   la que no se le pasó el log del finisher es un caso normal, no un
   error. El conteo final es global y no señala un mensaje; cuando el
   chequeo 1 está limpio, delata que faltan filas de log.

### Alternativas descartadas

- **Un script en Python o AWK.** Más corto de escribir, pero agrega una
  dependencia de runtime a la hora de evaluar y se aparta del resto del
  entregable, que es C con su propia convención de pruebas.
- **Verificar en vivo dentro del consumidor.** Mezclaría la herramienta de
  medición con lo medido (y su costo entraría en las latencias), y no
  podría cruzar eventos de procesos distintos.
- **Un parser CSV general, con comillas y escapes.** D12 ya garantiza que
  ningún campo lleva comas. Un parser tolerante escondería un logger roto
  en vez de delatarlo, que es lo contrario de lo que se busca acá.

### Limitaciones documentadas

- Una fila malformada invalida el archivo entero y no se indica en qué
  línea. Para un archivo de 10 000 filas con una corrupta es un
  diagnóstico pobre; se resuelve al agregar la CLI, que es la que tiene el
  nombre del archivo para reportar "archivo X, línea N".
- Las ramas de `ferror` y de falla de `realloc` no tienen prueba
  automatizada: forzarlas requiere inyectar fallos de E/S o de memoria, y
  los trucos portables para lograrlo no se comportan igual en Linux y en
  macOS.
- **El chequeo de contenido no recalcula el checksum**, solo compara el
  que registró el `PUT` contra el del `GET`. D1 lo habilita
  explícitamente — `fill[i] = (producer_id + sequence + i) % 256` y
  FNV-1a sobre 28 bytes hacen que el checksum sea una función pura de
  `(producer_id, sequence)` —, y recalcularlo detectaría dos cosas más:
  un productor que generó mal el payload desde el arranque, y un
  consumidor con un `payload_ok` mal calculado (hoy el verificador le
  cree).

  No se hizo en #7 porque **al 2026-10-19 no existe ninguna
  implementación del checksum en el repo**: solo están el campo en
  `struct payload` y la especificación de D1. El plan la espera en #14
  (`producer`, Emmanuel), que depende de #4, #5, #6, #11, #12 y #13, así
  que llega bastante después del verificador. El criterio de #10
  ("payload verificable (definido y documentado)") se cumplió con D1, sin
  código.

  **Lo que hay que no perder de vista:** #15 (`consumer`, Isaac) también
  necesita recalcular `fill` y `checksum` para poder escribir la columna
  `payload_ok` que exige D12, y sus criterios de aceptación no lo
  mencionan. Si el productor, el consumidor y el verificador escriben
  cada uno su FNV-1a, basta un byte de diferencia en la serialización
  para que el verificador marque *todos* los mensajes como alterados. La
  salida propuesta es una sola función compartida,
  `payload_checksum(producer_id, sequence)`, en un issue chico del que
  dependan #7 (para endurecer este chequeo), #14 y #15 — y corregir el
  criterio de #15 para que nombre el recálculo.
