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

### Códigos de salida y pruebas de la CLI (slice 4)

| Código | Significado |
|---|---|
| 0 | sin violaciones |
| 1 | al menos una violación |
| 2 | no se pudo leer una entrada, o el uso es incorrecto |

Separar el 1 del 2 importa: un archivo ilegible no es lo mismo que una
corrida incorrecta, y el #20 necesita distinguirlos para no dar por
fallada una prueba cuando lo que falló fue el arnés.

`verify` agrega a `BINS` en el Makefile. No es opcional: `COMMON` es
`src/*.c` menos los `main`, así que un `src/verify.c` fuera de `BINS`
terminaría enlazado dentro de los demás ejecutables y de cada binario de
prueba, con dos `main`.

Las pruebas van en dos niveles. `tests/test_verify.c` prueba la lógica
con CSV en memoria; `tests/test_verify_cli.c` corre el **binario de
verdad** con `popen` contra los CSV de `tests/fixtures/` y verifica el
código de salida y el texto del reporte, que son criterios de aceptación
del issue y no se pueden comprobar llamando a las funciones. Hay un
fixture por tipo de violación, uno correcto, uno malformado, y un par
(`multi_productor.csv` + `multi_consumidor.csv`) que solo cuadra cuando
se pasan los dos juntos — el caso que justifica la herramienta, ya que
cada proceso escribe su propio archivo.

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

- ~~Una fila malformada invalida el archivo entero y no se indica en qué
  línea.~~ **Resuelto en #7, slice 4.** `log_rows_read` pasó de devolver
  `bool` a un `enum log_read_result` (vacío, fila inválida, error de
  lectura, sin memoria) más el número de línea, por la misma razón que
  `memory_result` en D13: los casos que el usuario necesita distinguir no
  se corresponden uno a uno con un `errno`. La CLI imprime
  `verify: <archivo>, linea <N>: <motivo>`. La fila mala sigue
  invalidando el archivo entero, que es lo correcto —seguir con filas
  salteadas daría un veredicto sobre datos incompletos—, pero ahora dice
  dónde está.
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

---

## D17 — Logger CSV (`include/logger.h`)

- **Fecha:** 2026-10-20
- **Issue:** [#6](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/6)
- **Quién decide:** Isaac · **Revisa:** Emmanuel

### Contexto

D12 fija las columnas y qué campos lleva cada evento. El enunciado pide
*"capturar bajo protección los datos necesarios para registrar un evento y
escribirlos después"* y *"un CSV por proceso"*. El verificador (#7) lee
esos archivos, así que cualquier desvío de D12 lo rompe.

### Decisión

**Captura y escritura separadas.** `struct op_snapshot` (D3) ya trae la
secuencia, la marca de tiempo y la ocupación tomadas bajo el mutex. Después
de soltarlo, el proceso arma un `struct csv_record` con
`csv_record_put` / `csv_record_get` (solo copian campos) y recién entonces
llama a `csv_logger_write`, que hace la E/S:

```c
sync_put(inst, &msg, &snap);          /* captura bajo el mutex (D3) */
csv_record_put(&rec, &msg, &snap);    /* copia, sin mutex */
csv_logger_write(&lg, &rec);          /* E/S, sin mutex */
```

**El logger aplica D12 por su cuenta.** Una tabla interna dice, por evento,
qué roles lo pueden emitir y qué columnas lleva. Quien llama llena el
registro con lo que tiene; el logger escribe solo las columnas que D12
define para ese evento y deja vacías las demás. Así la regla vive en un
solo lugar y no en cada `fprintf` del producer, el consumer y el finisher.
Un evento que no corresponde al rol (un `PUT` en el log de un consumidor)
se rechaza con `-EINVAL` sin escribir nada.

**Formato de cada campo:**
- Enteros en decimal, `checksum` como `0x` + 8 dígitos hex en minúscula,
  `payload_ok` como `0`/`1`.
- `detail` se trunca a 47 caracteres (el mismo límite que lee el
  verificador) y sus comas, saltos de línea y caracteres de control se
  reemplazan por `_`, que romperían el CSV.
- Cada fila se arma completa en un buffer y se escribe con un solo
  `fwrite`, así una fila nunca queda partida entre dos escrituras.

**Archivo:** `csv_logger_open` crea o trunca el archivo y escribe la
cabecera; si falla, no queda nada abierto. Desde D32 trunca solo después
de tomar un candado de escritura: si otro proceso vivo escribe ese
archivo, devuelve `-EBUSY` sin tocarlo. Se usa el buffer de `stdio`
(no se vacía en cada fila) para que el log cueste lo menos posible durante
la corrida; `csv_logger_close` vacía y cierra, y reporta el error de una
escritura diferida, que con buffer aparece recién ahí. El enunciado no
pide conservar logs ante `SIGKILL`.

**Nombres con prefijo `csv_`:** el verificador ya usa `log_row`,
`log_event` y `log_*`. Todos los módulos de `src/` se enlazan en los mismos
binarios (D6), así que los nombres no pueden repetirse.

### Verificación cruzada con el verificador

Se generó con este logger una corrida completa y coherente (productor,
consumidor y finisher, 156 filas) y se pasó por el verificador de #7: sin
violaciones y con el cierre verificado. Con un mensaje alterado y uno
perdido sembrados a propósito, el verificador reportó los dos más la
inconsistencia del cierre que provoca la pérdida.

### Alternativas descartadas

- **Un `fprintf` por evento en cada ejecutable:** repetiría la tabla de
  D12 en tres lugares y un error en uno solo pasaría desapercibido hasta
  correr el verificador.
- **`fflush` en cada fila:** más robusto ante una caída, pero agrega una
  llamada al sistema por evento y distorsiona el experimento. El costo de
  los logs se discute en el informe, como pide el enunciado.

---

## D18 — Política de errores de las primitivas y cuarta condvar

- **Fecha:** 2026-10-20
- **Issue:** [#11](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/11)
- **Quién decide:** Emmanuel · **Revisa:** Isaac

> **Numeración:** D16 queda reservada para el verificador (PR #37) y D17
> para el logger (PR #39), ambos abiertos al escribir esto.

### Contexto

`sync_lock`, `sync_unlock` y `sync_broadcast_all` devuelven `void`: no
tienen por dónde informar un error. La revisión de #29 pidió decidir qué
hacer cuando una primitiva falla, y dejarlo escrito.

### Decisión

Dos tratos distintos, según quién pueda hacer algo con el error:

- **`sync_init_primitives` y `sync_destroy_primitives` devuelven `-rc`.**
  Las llaman el creator y el finisher, que todavía pueden informar al
  usuario y limpiar lo que crearon. Un fallo acá es una condición
  manejable: el segmento no se publica y listo.
- **`lock` / `unlock` / `wait` / `signal` / `broadcast` abortan**, con el
  nombre de la llamada y la causa en `stderr`. Los errores que estas
  primitivas pueden devolver describen un protocolo ya roto, no una
  condición de carrera recuperable: `EINVAL` es un mutex sin inicializar,
  `EPERM` es soltar un mutex ajeno, `EDEADLK` es un deadlock detectado.
  Continuar después de cualquiera de los tres significa operar sobre un
  buffer compartido cuya exclusión mutua no se sostiene, que es peor que
  terminar.

```c
static void primitive_failed(const char *call, int rc) {
    fprintf(stderr, "sync: %s: %s\n", call, strerror(rc));
    abort();
}
```

Dos detalles que es fácil equivocar:

- **Chequeo explícito, no `assert`.** Con `-DNDEBUG` el `assert`
  desaparece y el fallo pasaría en silencio. Es la misma lección del fix
  de `now_monotonic_ns` en #34.

### Cuándo un error de programación también aborta

Ampliación agregada en #13 a partir de la revisión de #41. Lo escrito
arriba se leía como "fallo de primitiva → `abort`, error de quien
programa → `assert`", y con esa lectura el `abort` de
`participant_leave` (D19) parecía contradecir esta entrada. No la
contradice: **el criterio no es quién se equivocó, sino qué pasa si el
error no se detecta en un build de release.**

- Los `assert` de `buffer.c` y `metrics.c` protegen cosas que, sin ellos,
  revientan igual al instante: un índice fuera de rango, un módulo por
  cero. Compilarlos afuera no esconde nada, el programa muere de todos
  modos.
- Un doble `participant_leave`, en cambio, descuenta `active_*` de más y
  **no revienta**: el finisher se queda esperando para siempre o cierra
  antes de tiempo. Sin mensaje, sin crash, con resultados mal.

Entonces: un error de quien programa también aborta, en vez de quedar en
un `assert`, cuando su consecuencia silenciosa en release sería un
cuelgue, un resultado incorrecto o una pérdida de datos, y no un crash
inmediato.
- **`strerror(rc)` y no `perror`.** A diferencia de las llamadas al
  sistema, las funciones `pthread_*` **no usan `errno`**: devuelven el
  código de error directamente. Usar `perror` ahí imprimiría el `errno`
  de alguna llamada anterior, sin relación con el fallo real.

**Sin prueba automatizada:** forzar que `pthread_mutex_lock` falle exige
corromper el mutex a propósito, y el comportamiento de hacerlo no está
definido por el estándar, así que una prueba de eso verificaría una
casualidad del sistema y no un contrato.

### Cuarta condvar: `closing`

`struct instance` gana una cuarta condvar, pedida también por la revisión
de #29. Mantiene la invariante que hace correcto usar `signal` en vez de
`broadcast` (D3): **cada condvar tiene un solo tipo de esperador con un
mismo predicado.**

| Condvar | Quién duerme | Predicado |
|---|---|---|
| `not_full` | productores | esperando espacio |
| `not_empty` | consumidores | esperando mensajes |
| `closing` | productores en pausa (#13) | esperando a que termine la pausa o el corte |
| `all_left` | el finisher | esperando `refs == 1` |

Sin ella, la pausa interrumpible de #13 tendría que dormir en `not_full`,
y entonces un productor en pausa podría consumir el `signal` destinado a
otro bloqueado por buffer lleno, que se quedaría dormido con espacio
disponible. `sync_broadcast_all` despierta las cuatro.

**`INSTANCE_VERSION` pasa de 1 a 2**, porque agregar la condvar cambia el
tamaño y el layout del segmento. `memory_open` valida la versión (D13),
así que un binario nuevo rechaza con `MEM_INCOMPATIBLE` una instancia
creada por uno viejo, en vez de interpretar mal sus campos. Es
exactamente para lo que existe ese campo.

---

## D19 — Implementación de la conexión y el registro (`src/participants.c`)

- **Fecha:** 2026-10-22
- **Issue:** [#12](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/12)
- **Quién decide:** Isaac · **Revisa:** Emmanuel

### Contexto

D8 (registro de por vida) y D9 (puerta de conexión) fijan el diseño. Esta
entrada registra lo que se agregó o precisó al implementarlo. Las
operaciones de cierre de `participants.h` (`closing_*`) quedan para #16 y
#17.

### Decisiones de la implementación

- **`instance_attach` = `memory_open` + CAS sobre `refs`.** Primero mapea y
  valida (D13) sin tocar primitivas; después toma la referencia con
  `atomic_compare_exchange_weak` mientras la puerta siga abierta. Con la
  puerta cerrada desmapea y devuelve `ATTACH_CLOSING`.
- **Dos resultados nuevos en `attach_result`:** `ATTACH_INVALID_NAME` y
  `ATTACH_INCOMPATIBLE`, para no perder la distinción que ya hace
  `memory_open` (D13). Se agregan `attach_result_str` y
  `register_result_str` para los mensajes de los ejecutables.
- **`sync_signal_all_left` en `sync.c`:** `instance_detach` tiene que
  despertar al finisher con `signal(all_left)` bajo el mutex (D9). Como
  `sync.c` es la única capa que toca condvars (D3), se agrega ahí esa
  función, con la política de errores de D18, en vez de llamar a
  `pthread_cond_signal` desde este módulo.
- **El registro sigue el orden de D8** (estado, duplicado, espacio) en una
  sola región crítica. La búsqueda de duplicados recorre también las
  casillas `SLOT_LEFT`: el id queda reservado de por vida. Un rechazo no
  escribe la tabla, los contadores ni `*slot`.
- **`participant_leave` aborta ante una casilla inválida o ya retirada.**
  Retirarse dos veces descontaría de más `active_*`, y el finisher podría
  cerrar antes de tiempo o esperar para siempre. Es un error de quien
  programa, así que sigue la política de D18 (mensaje y `abort`) en vez de
  devolver un código que nadie revisaría.

### Cómo se prueba la concurrencia

Además de los casos unitarios, `test_participants` corre procesos reales:

- **32 productores y 32 consumidores registrándose a la vez** con ids
  únicos: todos entran y cada id aparece una sola vez por rol.
- **16 procesos intentando registrar los mismos 64 ids**, soltados a la
  vez con una largada por pipe (crear procesos tarda lo suficiente para
  que, sin ella, casi nunca coincidan). Cada id tiene que quedar con un
  solo dueño.
- **El despertar del finisher:** la conexión del trabajador se toma antes
  del `fork` y el hijo la hereda, así el padre siempre llega a dormir en
  `all_left` con el predicado de D9.

Verificado por mutación: liberar el id al retirarse, quitar el mutex del
registro y quitar el `signal` de `instance_detach` hacen fallar,
respectivamente, `duplicate_after_leave`, `concurrent_duplicate_race` (3
de 3 intentos) y `detach_wakes_waiter` (por la alarma de 10 s).

---

## D20 — Pausa interrumpible por el cierre (`sync_pause_ms`)

- **Fecha:** 2026-10-22
- **Issue:** [#13](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/13)
- **Quién decide:** Emmanuel · **Revisa:** Isaac

### Contexto

El enunciado pide que el productor pause entre intentos para simular
trabajo, y que un cierre solicitado durante esa pausa lo saque de ella
sin insertar otro mensaje. D10 ya fijaba que la pausa duerme en la
condvar dedicada `closing` y que el `broadcast` del corte la interrumpe;
D11 fijó que ese tiempo **no** es espera de sincronización y no entra en
`wait_ns`. Falta la forma concreta.

### Decisión

```c
enum pause_result { PAUSE_COMPLETED = 0, PAUSE_INTERRUPTED };
enum pause_result sync_pause_ms(struct instance *inst, double ms);
```

- **Recibe la duración ya muestreada, no la media.** El muestreo
  exponencial es del productor (`rng_exponential_ms`, #5); así esta capa
  no depende del RNG y se prueba con duraciones fijas.
- **Espera con plazo sobre `closing`**, dentro de un `while` que revisa
  el estado y el vencimiento. `ETIMEDOUT` es el fin normal de la pausa, no
  un error; cualquier otro código sigue la política de D18. Un `rc == 0`
  puede ser un despertar espurio, y por eso el `while` reevalúa.
- **Si el corte llega junto con el vencimiento se informa interrumpida.**
  El productor no debe insertar, y `sync_put` lo rechazaría igual.
- **Con `ms` no positivo o NaN no espera.** El caso `--mean-ms 0` lo
  resuelve el llamador sin llamar acá (rng.h), pero dejar la función
  total evita un camino indefinido: convertir un `double` negativo a
  `uint64_t` es comportamiento indefinido en C.
- **El plazo se fija antes de tomar el mutex**, así la pausa se cuenta
  desde la llamada y no desde que se consiguió el bloqueo.

### El plazo tiene que leerse del reloj de la condvar

`pthread_cond_timedwait` no recibe una duración sino **un instante
absoluto**, y lo compara contra el reloj con el que se configuró la
condvar. `sync_init_primitives` solo llama a `pthread_condattr_setclock`
si `SYNC_HAS_CONDATTR_SETCLOCK`, que en macOS vale 0 (D13).

Si el plazo se calculara siempre con `CLOCK_MONOTONIC` —el reloj del
resto del proyecto— en macOS se le pasaría a una condvar de
`CLOCK_REALTIME` un instante de hace medio siglo: los dos relojes están
separados por unos 1,79 × 10⁹ segundos. `timedwait` devolvería
`ETIMEDOUT` de inmediato y **la pausa desaparecería sin un solo error**,
porque `ETIMEDOUT` es justo el código del fin normal.

Medido en Linux, pidiendo 300 ms y simulando cada configuración:

| Condvar | Plazo | Duración real |
|---|---|---|
| `CLOCK_MONOTONIC` | `CLOCK_MONOTONIC` | 300 ms |
| por defecto (macOS) | `CLOCK_MONOTONIC` | **0 ms** |
| por defecto (macOS) | `CLOCK_REALTIME` | 300 ms |

Por eso el reloj del plazo cuelga del **mismo** `#if` que decide el de la
condvar, para que no puedan desalinearse:

```c
#if SYNC_HAS_CONDATTR_SETCLOCK
#define SYNC_COND_CLOCK CLOCK_MONOTONIC
#else
#define SYNC_COND_CLOCK CLOCK_REALTIME
#endif
```

Costo aceptado en macOS: la pausa queda sensible a un cambio de hora del
sistema a mitad de corrida, que es justo lo que D3 quería evitar usando
`CLOCK_MONOTONIC`. Pero "la pausa se desvía si alguien cambia la fecha"
es preferible a "la pausa no existe", y solo afecta al entorno de
desarrollo local, no al de referencia (D13).

`test_pause_actually_waits` es la regresión de esto: mide que una pausa
de 120 ms dure al menos 100. Con el acoplamiento roto a propósito, falla.
Las pruebas de pausa llevan además una alarma, porque el desalineamiento
inverso (condvar monotónica, plazo en tiempo real) no acortaría la espera
sino que la volvería de décadas, y sin alarma `make test` colgaría.

---

## D21 — Corte a CLOSING: `closing_begin` y el snapshot del corte

- **Fecha:** 2026-10-22
- **Issue:** [#16](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/16)
- **Quién decide:** Emmanuel · **Revisa:** Isaac

> **Numeración:** D20 queda reservada para la pausa interrumpible
> ([#13](https://github.com/ebarrantes07/SOA-P2-GrupoIE/pull/42)), abierta
> al escribir esto.

### Contexto

D10 ya fijó el pseudocódigo de `closing_begin`: precondición, finalizador
único sin bandera extra, transición como punto de corte, cierre de la
puerta y `broadcast`. Esta entrada registra lo que la implementación tuvo
que agregar.

### El snapshot del corte: por qué cambia la firma declarada en D8

`participants.h` declaraba `closing_begin(struct instance *inst)`. No
alcanza, y el motivo es D12: la fila `CLOSING` del CSV lleva los
aceptados, los extraídos y los pendientes **en el punto de corte**.

Esos tres números solo existen dentro de la región crítica. Al soltar el
mutex, `accepted` queda congelado (ya no entran inserciones) pero los
consumidores **siguen drenando**, como manda D10: `extracted` crece y
`pending` baja. Hay dos formas de equivocarse al leerlos afuera, y fallan
distinto:

- **Sin el mutex** (lo natural, porque son miembros comunes del struct y
  el mutex acaba de soltarse): las lecturas no son atómicas entre sí. Si
  un consumidor extrae entre dos de ellas, los números quedan
  **mutuamente inconsistentes** y el chequeo de cierre del verificador
  (D16) reporta `cierre inconsistente` sobre una corrida correcta.
  Comprobado con dos CSV idénticos salvo esa fila: `10, 6, 4` pasa
  limpio; `10, 7, 4` da violación. Y es **intermitente**, porque depende
  de caer en una ventana de nanosegundos.
- **Volviendo a tomar el mutex:** quedan consistentes entre sí, porque
  `aceptados == extraídos + pendientes` es un invariante del buffer en
  todo instante, así que el verificador no se queja. Pero describen un
  momento posterior al corte: el CSV diría "4 pendientes al cortar"
  cuando había 7. Dato silenciosamente falso, que termina en el informe
  como cantidad medida.

De ahí el parámetro de salida:

```c
struct close_snapshot {
    uint64_t accepted, extracted, closing_ns;
    uint32_t pending;
};
enum close_result closing_begin(struct instance *inst,
                                struct close_snapshot *snap);
```

Mismo patrón que `struct op_snapshot` en D3, y por la misma razón:
capturar bajo protección y escribir después. **No** se resuelve retornando
con el mutex tomado (como sí hace `closing_wait_all_left`), porque
entonces el llamador escribiría el CSV reteniendo el mutex — justo lo que
el enunciado prohíbe, y bloquearía a todos los participantes durante una
escritura a disco.

`snap` admite `NULL`: el #17 no necesita los números, solo el ejecutable
al escribir la fila. Si no es `NULL` se pone en cero al entrar, así queda
definido en cualquier camino; un rechazo lo deja en cero, porque D12 no
pide números en la fila `CLOSE_REJECTED`.

### Las cuatro combinaciones de la precondición

La expresión de D10 es corta y fácil de leer mal, así que quedan
enumeradas:

| Consumidores activos | Productores activos | Pendientes | Resultado |
|---|---|---|---|
| ≥ 1 | cualquiera | cualquiera | `CLOSE_OK` |
| 0 | 0 | 0 | `CLOSE_OK` (no queda nada por drenar) |
| 0 | > 0 | cualquiera | `CLOSE_NO_CONSUMER` |
| 0 | 0 | > 0 | `CLOSE_NO_CONSUMER` (nadie sacaría esos mensajes) |

Un rechazo no reserva nada: la instancia sigue en OPEN, con la puerta
abierta y `closing_start_ns` en 0, así que se puede levantar un consumidor
y reintentar.

### Este issue no trae el ejecutable `finisher`

Los cinco criterios del #16 son sobre la lógica, y el binario necesita
además `closing_wait_all_left` y `closing_release` (#17) para completar el
cierre. Un `finisher` que pasara a CLOSING y terminara ahí dejaría la
instancia en CLOSING para siempre, con las primitivas vivas y el objeto de
`/dev/shm` presente: peor que no tener ejecutable. El binario llega con el
#17.

---

## D22 — Módulo del payload verificable (`include/payload.h`)

- **Fecha:** 2026-10-24
- **Issue:** [#44](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/44)
- **Quién decide:** Emmanuel · **Revisa:** Isaac

### Contexto

D1 especifica el payload verificable (`fill[i] = (producer_id + sequence + i)
% 256` y FNV-1a de 32 bits sobre 28 bytes serializados campo por campo) y D12
define la columna `payload_ok` como *"el consumidor recalculó `fill` y
`checksum` y coinciden"*. Hasta este issue eso existía solo como prosa:
`struct payload.checksum` estaba declarado, el logger lo copiaba y el
verificador comparaba `PUT.checksum` contra `GET.checksum`, pero **nadie lo
calculaba**.

Los dos extremos lo necesitan: el productor (#14) al armar el mensaje y el
consumidor (#15) al extraerlo. Esa dependencia no estaba registrada en el
plan ni en los issues; apareció al arrancar el #14 y por eso el #44 se abrió
después que los issues que desbloquea.

### Decisión

**Un solo módulo, `src/payload.c`, para los dos roles.** Con una
implementación por ejecutable, las dos tendrían que coincidir byte a byte o
todos los `payload_ok` saldrían en `0` en una corrida sana — un fallo que se
vería recién al leer los CSV del experimento y que se diagnosticaría como
corrupción de memoria compartida en vez de como dos hashes distintos.

**`payload_checksum` recibe `fill` por separado** en vez de recalcularlo a
partir de la identidad. El consumidor tiene que hashear el relleno que
*realmente viajó* en el mensaje; si la función lo regenerara internamente, un
`fill` corrupto produciría el mismo checksum que el sano y el chequeo no
detectaría nada.

**Serialización explícita y no un hash sobre la estructura.** `memcpy` de
`sequence` (8 bytes) y `producer_id` (4) a un buffer local, y después los 16
de `fill`. Hashear `&payload`/`sizeof(payload)` ataría el valor a cómo el
compilador decida alinear y rellenar los campos. `_Static_assert` sobre
`PAYLOAD_CHECKSUM_INPUT_LEN == 28` fija el número de D1 en el código.

#### Alcance de `payload_ok`: tres chequeos, no uno

| # | Chequeo | Qué detecta | Quién más podría detectarlo |
|---|---|---|---|
| 1 | Identidad del sobre contra las copias del payload | un mensaje cuyo payload pertenece a otra identidad | **nadie más**: el CSV de D12 tiene una sola columna `producer_id` y una sola `sequence`, así que si discrepan, el verificador ya no ve la diferencia |
| 2 | `fill` byte a byte contra el esperado para esa identidad | contenido cambiado **con el checksum rehecho** | nadie |
| 3 | Campo `checksum` contra el recalculado sobre el `fill` que viajó | el propio campo corrupto, o un payload nunca armado | el verificador, comparando `PUT.checksum` con `GET.checksum` |

Entre los tres cubren los 32 bytes del payload: ninguno puede cambiar, tomando
el sobre como referencia, sin que `payload_verify` devuelva `false`. El (2)
parece redundante con el (3) — un `fill` corrupto ya rompe el checksum — y no
lo es solo en un caso: contenido cambiado *y* checksum recalculado para que
cuadre. La prueba `fill_from_another_identity_is_detected` es la que lo fija;
sin ella el `memcmp` se podría borrar y las demás pruebas seguirían pasando.

El orden importa. El paso 3 hashea `p->fill` y no `expected`, que serían
equivalentes **mientras el paso 2 siga delante**; quitar el paso 2 y hashear
`expected` a la vez deja pasar cualquier `fill` corrupto. La prueba
`every_fill_byte_is_detected` cubre esa combinación.

Devuelve `bool` y no el motivo porque D12 define `payload_ok` como un bit. Los
tres chequeos son separables si alguna vez se quiere el motivo en `detail`.

### Vector congelado y por qué

El valor del checksum está fijo en la prueba (`{1, 1} -> 0xdf8b4fb5`, y cinco
más), calculado con una implementación independiente de FNV-1a escrita desde
esta bitácora y no con el código C que verifica. Sin el vector, un cambio de
algoritmo, de orden de campos o de fórmula del relleno no rompería ninguna
prueba: el ida y vuelta `build`/`verify` sigue cuadrando con cualquier hash
consistente consigo mismo. El síntoma aparecería recién en el experimento,
cuando los CSV de productor y consumidor dejaran de coincidir.

El vector asume el orden de bytes del host, porque D1 decide no normalizar
endianness (la ejecución es de un solo host). La prueba verifica
explícitamente que el host es little-endian, así en una máquina big-endian
falla esa condición con su nombre en vez de un hexadecimal sin contexto.

### Lo que midieron las mutaciones

Seis mutaciones de `src/payload.c`, cada una un error plausible:

| Mutación | Resultado |
|---|---|
| Serializar `producer_id` antes de `sequence` | rompe 3 pruebas |
| FNV-1 en vez de FNV-1a (multiplicar y luego XOR) | rompe 4 pruebas |
| Sin el chequeo cruzado de identidad | rompe `identity_mismatch_is_detected` |
| Sin el `memcmp` del `fill` | rompe `fill_from_another_identity_is_detected` |
| Suma del `fill` en 32 bits | **sobrevive: es equivalente** |
| Checksum sobre `expected` en vez de `p->fill` | **sobrevive: es equivalente dado el paso 2** |

Las dos sobrevivientes no son huecos de prueba sino equivalencias
semánticas, y la primera corrigió un comentario del código que afirmaba lo
contrario: el `% 256` solo mira el byte bajo de la suma, y el desborde de un
`unsigned` envuelve sin alterarlo, así que hacer la aritmética en 32 bits da
exactamente los mismos bytes. El `uint64_t` queda por claridad de lectura, no
por corrección, y el comentario ahora lo dice así.

---

## D23 — Ejecutable `consumer`

- **Fecha:** 2026-10-24
- **Issue:** [#15](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/15)
- **Quién decide:** Isaac · **Revisa:** Emmanuel

### Contexto

El consumer usa el módulo de payload de D22 (#44) para escribir
`payload_ok` (D12): después de cada `sync_get` llama a `payload_verify`,
fuera de la región crítica. No trae implementación propia del checksum.

### Flujo del consumer

1. **Valida argumentos** (D15) y **abre su CSV antes de conectarse**, para
   que también quede registro de un rechazo.
2. **Se conecta y se registra** (D9, D8). Un fallo escribe `JOIN_REJECTED`
   con el motivo de D12 (`not_found`, `closing`, `duplicate_id`,
   `not_open`, `table_full`) y sale con código 1, sin dejar recursos.
3. **Repite:** `sync_get` bloqueante (D3) → `payload_verify` (D22) → fila `GET`
   → procesamiento simulado → fila `PROCESSED`. Todo lo posterior a
   `sync_get` ocurre sin el mutex.
4. **`GET_DRAINED`** (CLOSING y buffer vacío, D10) → fila `DRAINED`,
   `participant_leave` con sus métricas, fila `LEAVE` (`drained`),
   `instance_detach`.

### Decisiones

- **El procesamiento simulado no se interrumpe con el cierre**, a
  diferencia de la pausa del productor (D20): el enunciado pide que el
  consumidor termine el procesamiento de lo que ya retiró. Es un
  `nanosleep` que se reanuda si una señal lo corta. Con `--mean-ms 0` no
  se llama al muestreo.
- **Un error de escritura del CSV no corta la corrida.** Se recuerda el
  primero y se reporta al final con código 1. Si el consumidor abandonara
  a mitad de camino, dejaría mensajes sin drenar y su referencia de
  conexión tomada, y el finisher esperaría para siempre.
- **La espera final en vacío se acumula** en `wait_ns`, como declara D11
  (espera combinada).
- **Código de salida con payloads alterados: 0.** El código de salida dice
  si el consumer cumplió el protocolo (se registró, drenó y se retiró), no
  si el contenido llegó sano. Un payload alterado queda en el CSV con
  `payload_ok = 0` y en el resumen de stdout, y la compuerta es el
  verificador (D16), que sí sale con 1. El script del experimento (#24)
  corre `verify` sobre los CSV de cada corrida y usa su código, no el de
  los ejecutables. Mezclar las dos cosas haría que un consumer correcto
  "fallara" por un error que no es suyo. (Pregunta de la revisión de #46.)
- **No se usan señales de terminación como mecanismo normal de salida**
  (el enunciado lo prohíbe): el consumer termina solo por `GET_DRAINED` o
  por un rechazo al entrar.

### Pruebas de punta a punta

`test_consumer` lanza `./consumer` como proceso real; la prueba hace de
productor y de finisher con sus propios CSV, y al final pasa los archivos
por `./verify` (D16). El payload de cada mensaje se arma con
`payload_build` (D22), igual que lo hará el producer. Cubre: corrida verificada, espera bloqueada con el
buffer vacío, drenaje completo después del corte (con pendientes en el
snapshot), cuatro consumidores, id duplicado, instancia inexistente,
conexión después del cierre y payload alterado.

Dos cosas no se verifican contando filas (revisión de #46):
- **El procesamiento después del corte se mide en tiempo:** para cada
  mensaje extraído después de `closing_ns`, la diferencia entre su `GET` y
  su `PROCESSED`. Si el cierre truncara el procesamiento, las filas
  `PROCESSED` se escribirían igual pero esa suma sería ~0.
- **Las métricas de la casilla** (`ops`, `wait_ns`, `wait_count`,
  `lock_ns`, que lee el resumen de #18) se revisan después de cada corrida.

Verificado por mutación: dejar `payload_ok` siempre en 1, salir al ver el
buffer vacío, salir al ver CLOSING sin drenar, saltear el procesamiento
fuera de OPEN y no pasarle las métricas a `participant_leave` hacen fallar
las pruebas correspondientes.

---

## D24 — Ejecutable `producer`

- **Fecha:** 2026-10-24
- **Issue:** [#14](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/14)
- **Quién decide:** Emmanuel · **Revisa:** Isaac

### Contexto

El productor es el otro extremo del payload de D22 (#44): arma `fill` y
`checksum` con `payload_build` antes de insertar, y el consumidor los
recalcula con `payload_verify`. Es también el primer proceso que usa la
pausa interrumpible de D20 para lo que fue diseñada.

### Flujo

1. **Valida argumentos** (D15) y **abre su CSV antes de conectarse**, para
   que un rechazo también quede registrado (mismo orden que D23).
2. **Se conecta y se registra** (D9, D8). Un fallo escribe `JOIN_REJECTED`
   con el motivo de D12 y sale con 1, sin dejar recursos.
3. **Repite:** pausa interrumpible (D20) → `payload_build` → `sync_put`
   bloqueante (D3) → fila `PUT`. La fila se escribe fuera del mutex, con
   lo capturado en el `op_snapshot`.
4. **Se retira** con `participant_leave`, fila `LEAVE` (`quota_done` o
   `closing`) e `instance_detach`.

### Decisiones

**La pausa va antes de cada intento, y es la que nota el cierre.** Con
`--mean-ms 0` no se llama al muestreo ni se duerme (lo pide el enunciado y
lo repite D20), así que ahí el corte se nota recién en `sync_put`. Eso
produce dos comportamientos distintos y los dos son correctos:

| Escenario | Filas `PUT_REJECTED` del productor | Por qué |
|---|---|---|
| `--mean-ms 5`, corte a los 400 ms | **0** (medido: 72 aceptados) | el corte lo despierta en la pausa; se retira sin intentar |
| `--mean-ms 0`, corte a los 400 ms | **1** (medido: 164 429 aceptados) | nunca duerme, así que el rechazo del `put` es su primera noticia |

Consecuencia para D11: `metrics.rejected_puts` **no mide trabajo perdido**.
Vale 0 o 1 por productor — es el intento con el que cada uno descubre el
corte, no una cuenta de mensajes que se cayeron.

**Al despertar interrumpido no intenta insertar.** El criterio del issue es
*"al notar CLOSING, se retira sin insertar otro mensaje más"*. Podría
llamar a `sync_put` y dejar que lo rechace, pero eso agregaría una fila
`PUT_REJECTED` que no aporta nada: ya sabe la respuesta.

**Un intento rechazado consume un número de secuencia.** La identidad se
asigna antes del `put` (`msg.sequence = ++sequence`), así que la secuencia
del intento rechazado queda escrita en la fila `PUT_REJECTED` —como pide
D12— y nunca aparece como `PUT`. El verificador (#16) cruza `PUT` contra
`GET`, no `PUT_REJECTED`, así que no lo ve como un mensaje perdido.
Verificado: las corridas con un `PUT_REJECTED` pasan `verify` sin
violaciones.

**La cuota cuenta aceptados, no intentos**, y no alcanzarla no es un error.
Medido: con `--messages 100000000` y un corte a los 300 ms, el productor
acepta 99 345, escribe `LEAVE closing` y **sale con 0**. El código de
salida tiene que distinguir "falló" de "el cierre ordenado llegó primero",
porque el script del experimento (#24) los lee: un productor interrumpido
por el finisher hizo exactamente lo que debía.

**El mensaje se pone en cero completo** (`memset`) y no campo por campo: el
`reserved` de `struct message` viaja a la memoria compartida y D13 lo quiere
siempre en 0.

**`rng_init` se revisa aunque hoy no pueda fallar.** La CLI exige `--seed`
en 1..UINT32_MAX, pero si alguna vez deja de exigirlo, `rng_init` devuelve
−1 **sin tocar la estructura** y seguir usaría memoria sin inicializar. Es
error de programación: aborta (D18).

**Un error de escritura del CSV no corta la corrida.** Se recuerda el
primero y se reporta al final con código 1. Abandonar a mitad de camino
dejaría la referencia de conexión tomada y el finisher esperaría para
siempre.

### Verificación de punta a punta (manual, hasta que exista el finisher)

Sin binario `finisher` (#17) todavía, las corridas se hicieron con un
cortador mínimo que llama a `closing_begin`. Resultados:

| Escenario | Resultado |
|---|---|
| Cuota de 50, un consumidor | 50 `PUT` / 50 `GET` / 50 `PROCESSED`, `verify` sin violaciones |
| Sin cuota, `--mean-ms 0`, corte a 400 ms | 164 429 aceptados, 1 rechazado, 493 295 filas, `verify` sin violaciones |
| Sin cuota, `--mean-ms 5`, corte a 400 ms | 72 aceptados, 0 rechazados, `verify` sin violaciones |
| Cuota inalcanzable + corte | 99 345 aceptados, `LEAVE closing`, código 0 |
| Instancia inexistente / id duplicado / instancia cerrándose | código 1 y `JOIN_REJECTED` con `not_found` / `duplicate_id` / `closing` |

**El cruce que importa para D22:** en las 164 429 extracciones de la corrida
larga, `payload_ok` dio 1 en todas. Es la primera validación entre procesos
del módulo del #44 — productor que arma y consumidor que recalcula, con una
sola implementación de FNV-1a de por medio.

Un hallazgo del camino: un cortador sobre una instancia con un mensaje sin
consumir y ningún consumidor registrado es rechazado con
`CLOSE_NO_CONSUMER`, y la instancia sigue OPEN. El productor que entra
entonces se bloquea esperando espacio, como manda D3. La precondición de
D10 hace justo lo que debe.

### Higiene de las pruebas: el segmento sí, los CSV no

`CHECK` devuelve `false` en el acto, así que una prueba que falla a mitad se
saltea su limpieza. Medido forzando una falla: queda un `t14_*` colgado en
`/dev/shm` hasta el próximo reinicio, y acumularlos mientras se depura es
fácil. `tests/test_producer.c` anota cada nombre que entrega y los borra con
`atexit`, que corre igual cuando una prueba falla.

Los CSV de `build/` se dejan a propósito en ese caso: son la evidencia de lo
que pasó, y `make clean` los borra. El segmento de memoria compartida no
sirve para depurar y sí molesta — de ahí la asimetría.

El mismo patrón está en `tests/test_consumer.c` (#15): una prueba suya que
falla deja su propio segmento. Se le pasó la nota en la revisión del PR.

---

## D25 — Probar que la espera es bloqueante y no por sondeo (`tests/test_blocking.c`)

- **Fecha:** 2026-10-26
- **Issue:** [#21](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/21)
- **Quién decide:** Emmanuel · **Revisa:** Isaac

### Contexto

El enunciado pide los casos "buffer lleno" y "buffer vacío", y agrega un
requisito que no se parece a los otros: *"Ambos casos verifican que la
espera es **bloqueante (sin CPU ocupada en sondeo)**, no solo que
eventualmente avanza."*

Esa distinción importa porque una espera por sondeo —`while (lleno) { suelto
el mutex y lo vuelvo a tomar; }`— **también** avanza cuando aparece la
contraparte. Pasaría cualquier prueba que mire solo el progreso, y sin
embargo es exactamente lo que el enunciado prohíbe.

### Decisión

Cada caso mide **tres** cosas del proceso que espera, durante una ventana de
600 ms:

1. que siga vivo (`waitpid` con `WNOHANG`),
2. que su contador compartido no se mueva (`enqueue_seq` o `dequeue_seq`,
   leídos bajo el mutex),
3. que **su CPU no crezca** y su estado sea de espera.

Lo tercero se lee de `/proc/<pid>/stat`: `utime + stime` (campos 14 y 15) y
el estado (campo 3). Se parsea desde el **último** `)` y no por número de
campo desde el principio, porque el campo 2 es el nombre del ejecutable
entre paréntesis y puede contener espacios.

### Por qué `/proc` y no una API POSIX

| Alternativa | Por qué no |
|---|---|
| `wait4(pid, …, &rusage)` | Da la CPU de **ese** hijo, que es justo lo que hace falta, pero **no está declarada bajo `_POSIX_C_SOURCE=200809L`** (comprobado: `-Werror=implicit-function-declaration`). Habría que relajar la macro de características de todo el proyecto (D5) por una prueba. |
| `getrusage(RUSAGE_CHILDREN)` | Sí compila con las flags del proyecto, pero **agrega todos los hijos ya cosechados** y solo informa **después** de que terminan. No puede observar a un proceso *mientras* está bloqueado, que es el momento que interesa. |
| Medir solo el progreso | Es lo que el enunciado descarta explícitamente: un sondeo también progresa. |

`/proc` es lo único que da la CPU de otro proceso **durante** su espera.
Depende de Linux, que es el entorno objetivo declarado en el README. Si
`/proc/<pid>/stat` no se puede leer, la prueba **falla con un mensaje que
nombra la limitación**, no se saltea en silencio: una prueba que pasa sola
cuando no puede medir es peor que no tenerla.

### Los números

Con `CLK_TCK = 100` (tick de 10 ms) y una ventana de 600 ms, se toleran **2
ticks** (20 ms, 3.3 % de la ventana) para absorber una falta de página o un
despertar espurio. Lo medido:

| Implementación | CPU del proceso que espera |
|---|---|
| Real (`pthread_cond_wait`) | **0 ticks**, estado `S` |
| Mutada a sondeo (`unlock`/`lock` en ciclo) | **51–60 ticks**, estado `R` |

Margen de 25x entre lo tolerado y lo que produce un sondeo. Las dos
mutaciones —sondeo en `sync_put` y en `sync_get`— hacen fallar su caso con
el diagnóstico exacto ("consumio 52 ticks de CPU en 600 ms de pared: parece
sondeo"), que es la prueba de que el criterio está cubierto y no solo
declarado.

### Dos productores y dos consumidores, no uno

El criterio habla en plural. Con un solo proceso no se demuestra que
**todos** los bloqueados duerman, así que cada caso levanta dos y los mide
en la **misma** ventana (una sola espera de 600 ms), para que agregar
procesos no alargue la prueba.

### Lo primero que corre con los dos ejecutables reales

A diferencia de `test_producer.c` y `test_consumer.c`, donde el proceso de
prueba hacía de contraparte, acá **los dos extremos son los binarios de
verdad** y la prueba solo observa y corta. Al final pasa los CSV por
`./verify`.

**Advertencia para quien corra las pruebas a mano:** el guardia de entrada
comprueba que `./producer` y `./consumer` existan y sean ejecutables, no que
estén recompilados. Correr `./build/test_blocking` directamente después de
cambiar `src/` puede medir binarios viejos; `make test` los reconstruye
primero. (Me pasó durante este issue: una mutación "sobrevivió" porque el
ejecutable no se había relinkeado.)

---

## D26 — Finisher II: espera de retiradas, liberación y ejecutable `finisher`

- **Fecha:** 2026-10-26
- **Issue:** [#17](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/17)
- **Quién decide:** Isaac · **Revisa:** Emmanuel
- **Numeración:** D25 es de las pruebas de bloqueo (#48), abierto antes que este PR.

### Contexto

D9 y D10 fijan el protocolo y D21 implementa el corte (`closing_begin`).
Faltaba la segunda mitad: esperar a que nadie más pueda tocar las
primitivas, liberarlas y el ejecutable que conduce todo el cierre.

### Decisiones

- **`closing_wait_all_left` espera sobre `refs`, no sobre `active_*`.**
  El predicado es el de D9: queda alguien conectado además del finisher.
  Cubre a quien se conectó y todavía no se registró, que va a recibir
  `REG_NOT_OPEN`, se va a desconectar y eso lo libera. Verificado por
  mutación: esperar solo a los registrados hace que ese proceso intente
  registrarse sobre un mutex ya destruido y muera.
- **`sync_wait_all_left` en `sync.c`**, pareja de `sync_signal_all_left`
  (D19): la espera sobre la condvar va en la única capa que las toca (D3).
- **Con `refs == 1` los contadores de activos deberían estar en 0.** Si
  no, alguien se desconectó sin retirarse; el finisher lo avisa por
  `stderr` y libera igual, porque dejar la instancia a medio cerrar sería
  peor.
- **`closing_release`**: `state = CLOSED`, suelta el mutex, destruye las
  primitivas y hace `shm_unlink`. No desmapea (el resumen lee el segmento
  después). Si algo falla sigue liberando y devuelve el primer error.
- **La duración del cierre** va de `closing_ns` (el snapshot del corte)
  hasta que `closing_release` retornó, como define D11.

### El ejecutable

Orden de D10:

```text
instance_attach → CLOSE_REQUEST → closing_begin(&snap) → CLOSING
→ closing_wait_all_left → ALL_LEFT → closing_release → CLOSED
→ resumen → munmap
```

- **Rechazos:** si la precondición falla (`no_consumer`) o ya hay un
  cierre en curso (`already_closing`, detectado en `closing_begin` o
  antes, por la puerta cerrada al conectarse), escribe `CLOSE_REJECTED`
  y sale con 1 sin tocar la instancia. Con `no_consumer` queda OPEN y se
  puede reintentar tras levantar un consumidor.
- **Consola y CSV:** cada evento se imprime siempre en la consola; con
  `--log` también va a su CSV (D12). Un error de escritura no corta el
  cierre y se reporta al final con código 1.
- **Resumen provisional.** `--summary` es obligatoria, pero
  `summary_write_csv` es del Issue #18. Mientras tanto, el finisher
  escribe en el formato largo de D11 las filas de instancia que ya tiene:
  `accepted`, `extracted`, `pending`, `rejected_puts`, `invariant_ok`,
  `close_duration`, `producers` y `consumers`. #18 reemplaza esa función
  por `summary_write_csv` y agrega esperas, latencia, p95, throughput y
  las filas por participante.

### Pruebas

`test_finisher` lanza `./finisher` como proceso real: cierre sin
trabajadores, espera de un participante registrado y de una conexión en
curso (en los dos casos `ALL_LEFT` es posterior a la desconexión del
participante), rechazo sin consumidor (la instancia sigue OPEN con la
puerta abierta, y tras retirarse el productor el cierre procede), segundo
finisher rechazado y ciclo completo con `./producer` cerrado, recreado con
el mismo nombre y vuelto a cerrar, con los CSV verificados por `./verify`
en los dos ciclos.

Verificado por mutación: no esperar las retiradas, esperar solo a los
registrados y no hacer `shm_unlink` hacen fallar las pruebas
correspondientes.

Además, con los cuatro ejecutables reales y el consumer del #46: 4
productores y 4 consumidores con cierre al terminar las cuotas, y cierre
en plena actividad con capacidades 1, 8 y 64 (el corte encontró el buffer
lleno en los tres casos). En todas las corridas `verify` no reportó
violaciones y `/dev/shm` quedó vacío.

---

## D27 — Script del experimento (`scripts/experimento.py`)

- **Fecha:** 2026-10-27
- **Issue:** [#24](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/24)
- **Quién decide:** Emmanuel · **Revisa:** Isaac
- **Traspaso:** el #24 era de Isaac; se movió a Emmanuel para no atrasar el
  cronograma mientras Isaac termina el #19, el #22 y el #26.

### Contexto

El enunciado pide 45 corridas (3 capacidades × 3 escenarios de medias × 5
repeticiones) con 4 productores y 4 consumidores de 200 mensajes cada
productor, conservando las semillas, pidiendo el cierre cuando terminan los
productores y guardando los CSV con nombres que identifiquen la combinación.

### Python y no shell

`scripts/fnv_ref.py` ya establecía el precedente de herramientas en Python
(solo biblioteca estándar; el programa sigue siendo C/POSIX puro). Pero la
razón de fondo es específica: un orquestador de 45 corridas que lanza 360
procesos y corre desatendido no es lugar para la cantidad de trampas que
tiene el shell con procesos en segundo plano. Durante este proyecto, tres
errores propios lo demostraron:

- un `wait $(jobs -p | head -2)` que esperaba a los procesos equivocados y
  dejó que el shell matara consumidores **en medio de un `fprintf`**: el CSV
  quedó terminando en `consumer,1,GET,82` sin salto de línea (el verificador
  lo detectó, que es justo el peligro que #16 cubre);
- un `pgrep -f "consumer --name /..."` que **se encontró a sí mismo**,
  porque el patrón aparece en la línea de comandos del propio shell, y mató
  la sesión;
- un `"/dev/shm${N#/}"` sin la barra, que generaba `/dev/shmp2_kill_z` y
  dejaba el segmento sin borrar.

`subprocess` con `wait(timeout=...)` y listas de argumentos no tiene ninguno
de esos filos.

### Una corrida vencida es inválida, no una corrida con datos

Si los productores no terminan dentro del plazo, la corrida se marca
`invalida`, se registra el motivo y **no se lanza el finisher**. Esto último
no es cosmético: con un participante muerto (o matado por el plazo), su
referencia en `refs` nunca se suelta y `closing_wait_all_left` espera para
siempre — medido en la revisión del PR #49 y anotado en el #50. Lanzar el
finisher ahí colgaría la corrida entera del experimento.

El plazo es generoso a propósito: lo que tardaría un solo proceso haciendo
todo el trabajo en serie, por 4, con un piso de 30 s.

### Teardown explícito de todo lo lanzado

Toda corrida que no termina bien mata lo que siga vivo y lo deja anotado en
el manifiesto (`[4 proceso(s) terminado(s) a la fuerza]`).

Hizo falta por una razón medida, no supuesta: la primera versión mataba a
los productores vencidos pero **dejaba a los consumidores**, que se quedan
esperando en `not_empty` mientras la instancia siga en OPEN — y el corte no
llega nunca, porque justamente no se lanza el finisher. Inyectando un plazo
imposible, cada lote de 3 corridas fallidas dejaba **12 consumidores vivos**
(4 por corrida), y sobrevivían entre invocaciones. Con las 45 corridas
fallando serían 180 procesos colgados. Con el arreglo: 0.

### Semillas reproducibles

Cada proceso recibe `(base + indice_de_corrida * 1000 + desplazamiento_de_rol
+ i) % (2^32 - 1) + 1`. El `+ 1` final garantiza que nunca sale 0, que
atasca `xorshift32` y la CLI rechaza (ver `rng.h`). Repetir el experimento
con la misma `--base-seed` reproduce las mismas secuencias de pausas.
Verificado: **360 semillas distintas para 360 procesos, ninguna repetida,
ninguna igual a 0.**

### El manifiesto es la interfaz con el #25

`results/manifiesto.csv` trae una fila por corrida con sus parámetros, sus
semillas, los códigos de salida, el resultado del verificador, la duración y
los totales de aceptados y extraídos. El análisis (#25) itera sobre ese
archivo en vez de deducir combinaciones de nombres de directorio, y se
escribe con `flush` tras cada corrida, así sigue siendo útil si el
experimento se interrumpe.

Los CSV de cada corrida van a `results/<id>/` con
`id = cap{C}_mp{MP}_mc{MC}_rep{R}`. `results/` está en `.gitignore` salvo
`seleccionados/` (D7), así que las 9.8 MB de salida no entran al repositorio.

### Orden de arranque

Los consumidores se lanzan primero: al cortar hace falta al menos uno
activo o `closing_begin` rechaza el cierre con `CLOSE_NO_CONSUMER`
(precondición de D10). Además, a los 200 ms se revisa que ninguno haya
terminado ya: un consumidor que muere al arrancar (ruta de log inválida, por
ejemplo) haría que el cierre se rechace mucho después, con la corrida
entera gastada.

### Resultados de la primera corrida completa

**45/45 corridas válidas en 70 s**, todas con el verificador en 0 y con
`accepted == extracted == 800` (4 productores × 200 mensajes).

Un hallazgo para el #25: **la duración de la corrida casi no distingue la
capacidad** (1.65–1.83 s en todas las combinaciones), porque el tiempo lo
domina la suma de las pausas exponenciales y no la contención del buffer.
Donde la capacidad sí aparece es en la latencia de inserción a extracción,
que es la métrica que el enunciado pide. En el escenario 2/8 (productores
rápidos, consumidores lentos, buffer que se llena), con 4000 muestras por
capacidad:

| Capacidad | Latencia media | p95 |
|---|---|---|
| 1 | 1.94 ms | 6.04 ms |
| 8 | 16.02 ms | 26.38 ms |
| 64 | 127.31 ms | 159.89 ms |

Es el resultado clásico del buffer acotado: un buffer más grande absorbe el
desbalance y, a cambio, los mensajes esperan más en cola. El experimento
produce la señal que el análisis necesita.

---

## D28 — Resumen final del finisher (`summary_write_csv`)

- **Fecha:** 2026-10-28
- **Issue:** [#18](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/18)
- **Quién decide:** Emmanuel · **Revisa:** Isaac

### Contexto

D11 fijó qué se mide y en qué formato se reporta, y el #8 implementó los
cálculos (histograma, media, percentil, throughput). Lo que faltaba era el
escritor: `summary_write_csv` estaba **declarado en `metrics.h` sin cuerpo
en ningún archivo**, y el `finisher` del #17 traía un
`write_provisional_summary` marcado explícitamente como provisional, con
solo los totales del corte.

Esta entrada implementa el formato completo de D11 y retira el provisional.

### Lo que cambia

`src/metrics.c` gana `summary_write_csv`, y `src/finisher.c` pierde sus 27
líneas de resumen provisional. El sitio de llamada y el manejo de errores
del ejecutable no cambian: el provisional ya devolvía `0` o `-errno` y se
reportaba al final, así que el reemplazo es de una línea.

Las 13 filas de instancia y las 5 por participante son exactamente las de la
tabla de D11. Se escribe **después** de `closing_release` y **antes** del
`munmap`, que es el orden que D10 fija para el finisher: el segmento ya no
tiene a nadie conectado, así que se lee sin mutex.

### Los tres filos del formato

**El percentil va en por ciento, no en fracción.**
`lat_hist_percentile_ns` toma `p` en `(0, 100]` y lo verifica con un
`assert`. Pasar `0.95` en vez de `95.0` **no dispara el assert**: calcula el
percentil 0.95, que es casi el mínimo. Con `-DNDEBUG` tampoco habría assert
que ayudara. El riesgo es que el número se vea plausible y esté mal por dos
órdenes de magnitud. La prueba `summary_p95_is_percent` lo fija: con
latencias de 1 a 100 ms, exige que el p95 caiga sobre los 80 ms.

**Ninguna nota puede contener una coma.** El CSV de D12 no usa comillas, así
que una coma en `note` partiría la fila en 7 campos y el lector vería basura.
Las notas usan punto y coma, y `summary_load` en la prueba cuenta las comas
de cada línea: una nota con coma rompe dos pruebas.

**Un participante que no se retiró no tiene `left_ns`.** Calcular
`left_ns - joined_ns` sobre una casilla activa da un número enorme, porque
la resta de enteros sin signo envuelve. La fila `lifetime` informa 0 con la
nota `no se retiro; sin duracion valida`. No debería pasar —el finisher avisa
si quedan activos— pero el resumen es justo el lugar donde se vería, y un
número absurdo es peor que un cero explicado.

### Esperas combinadas, declaradas (tercer criterio del issue)

El criterio dice que una medición que combina causas se documente así en vez
de presentarse como exclusiva de una. Va en la columna `note`:

| Fila | Nota |
|---|---|
| `wait_data` (consumidor) | `espera por datos en not_empty; incluye la espera final hasta el cierre` |
| `wait_space` (productor) | `espera por espacio en not_full; no incluye la pausa simulada` |
| `lock_acquire` | `incluye la readquisicion al despertar de cond_wait` |
| `latency_p95` | `aproximada por histograma; error <= 6.25 %` |
| `rejected_puts` | `intentos rechazados por el corte; no son aceptados` |

La de `rejected_puts` importa más de lo que parece: por D24, vale 0 o 1 por
productor —es el intento con el que cada uno descubre el corte— y **no mide
trabajo perdido**.

### Medido

Corrida real con 3 productores y 3 consumidores de 300 mensajes, pausas de
2 ms:

```text
accepted,instance,0,900,msgs,
extracted,instance,0,900,msgs,
invariant_ok,instance,0,1,bool,1 si accepted = extracted + pending y pending = 0
latency_mean,instance,0,3029063.621111,ns,exacta
latency_p95,instance,0,7077888,ns,aproximada por histograma; error <= 6.25 %
throughput,instance,0,1187.185592,msgs_per_s,primer inicio de trabajador a ultima extraccion
```

La suma de `ops` de los consumidores da los 900 extraídos de la instancia;
la prueba `summary_participant_rows` lo exige como invariante.

Cinco mutaciones, todas detectadas: percentil como fracción, nombres de
espera intercambiados entre roles, una coma en una nota, `lifetime` sin el
guardia, e `invariant_ok` fijo en 1.

El manifiesto del experimento (D27) lee `accepted` y `extracted` por nombre
de métrica, así que el formato nuevo le sirve sin cambios.

---

## D29 — Verificación final contra el enunciado

- **Fecha:** 2026-10-28
- **Issue:** [#53](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/53)
- **Quién decide:** Isaac · **Revisa:** Emmanuel

### Contexto

Antes de la entrega se recorrió el enunciado frase por frase contra el
código. Todos los casos de la tabla "Pruebas mínimas de funcionamiento"
pasaron con los ejecutables reales (incluidos estrés 32+32 y cierre en plena
actividad con ASan y UBSan), pero aparecieron cuatro puntos donde el texto
del enunciado no se cumplía al pie de la letra.

### Decisiones

- **Ninguna escritura con el mutex tomado.** `closing_wait_all_left`
  retorna con el mutex tomado (D9) y el finisher escribía la fila
  `ALL_LEFT` antes de `closing_release`. Ahora, con el mutex tomado, solo
  captura la marca de tiempo y los contadores de activos; la consola, el
  CSV y el aviso de activos sin retirar se escriben después de soltarlo.
  `closing_wait_all_left` no hace E/S. Es el mismo patrón de "capturar
  bajo protección y escribir después" que siguen `sync_put`/`sync_get`
  (D3) y el corte (D21). La fila `CLOSED` usa la misma marca con la que se
  mide la duración del cierre.
- **`clock_gettime` revisado en la pausa del productor**
  (`deadline_from_now`), con `perror` + `abort` como `now_monotonic_ns`
  (#34): un plazo basura haría que la pausa durara cualquier cosa sin
  aviso.
- **Estado protegido y propiedad de recursos documentados en `memory.h`**,
  junto a `struct instance`: qué campos protege el mutex, cuáles son
  atómicos (`magic`, `refs`), cuáles son inmutables después de publicar
  (`version`, `capacity`), y quién crea y quién destruye cada recurso
  (nombre, objeto, mapeo, primitivas, referencia de conexión, casilla de
  registro y CSV).
- **README con la lista obligatoria del enunciado:** entorno probado
  (además del objetivo), protocolo de ejecución y cierre paso a paso, uso
  de `verify`, formatos CSV de los logs y del resumen, y licencia del
  material reutilizado.

### Fuera de alcance

El límite de 64 registros por rol durante la vida de la instancia (D8)
queda como está: cumple el mínimo de 32 del enunciado y es una decisión de
diseño documentada, con un rechazo claro (`REG_TABLE_FULL`, código 1).

---

## D30 — Pruebas de conservación y escala: un arnés y cuatro corridas (`tests/test_concurrency.c`)

- **Fecha:** 2026-10-28
- **Issue:** [#20](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/20)
- **Quién decide:** Emmanuel · **Revisa:** Isaac

### Contexto

El enunciado pide cuatro casos de la tabla de pruebas mínimas —FIFO básico,
concurrencia, capacidad mínima y estrés— que no se diferencian en *qué* hay
que comprobar, sino en *con cuántos procesos*: en los cuatro, lo que no
puede pasar es perder, duplicar, alterar o desordenar un mensaje. Por eso
#7 bloquea a #20 (D16): el verificador existe para que estas pruebas puedan
afirmar algo más fuerte que "no crasheó".

### Decisión

Un solo arnés parametrizado (`run_start` / `run_join` / `run_verify`) y
cuatro casos de una línea. Cada corrida levanta `./producer` y `./consumer`
de verdad, con cuota finita para que termine sola, y afirma contra **dos
fuentes independientes**:

1. los contadores de la instancia leídos **en una sola región crítica**
   (`read_counters`): `enqueue_seq == N × cuota` al retirarse los
   productores, y `dequeue_seq == N × cuota`, `count == 0`,
   `active_consumers == 0` al terminar los consumidores. Dos lecturas
   sueltas afuera del mutex quedarían mutuamente inconsistentes, por lo
   mismo que `struct close_snapshot` (D21);
2. `./verify` sobre todos los CSV de la corrida.

| Caso | P | C | Capacidad | Cuota | Mensajes | CSV | Filas de log |
|---|---|---|---|---|---|---|---|
| FIFO básico | 1 | 1 | 8 | 1000 | 1 000 | 3 | 3 006 |
| Concurrencia | 4 | 4 | 8 | 1000 | 4 000 | 9 | 12 021 |
| Capacidad mínima | 4 | 4 | **1** | 1000 | 4 000 | 9 | 12 021 |
| Estrés | 32 | 32 | 8 | 1000 | 32 000 | 65 | 96 161 |

La cuota es 1000 en los cuatro para que entre concurrencia y capacidad
mínima la **única** variable sea la capacidad. Con un solo consumidor
(FIFO básico) el orden de extracción es un orden total, así que el chequeo
FIFO de `./verify` queda en su forma más estricta: cada mensaje sale en la
misma posición en que entró. El estrés usa 32 por rol, el mínimo del
enunciado y la mitad del límite de 64 casillas por rol (D8).

### El corte lo hace la prueba, no `./finisher`

Como en `test_blocking.c` (D25), el paso a CLOSING se hace con
`closing_begin` dentro del proceso de prueba y se escribe su fila CLOSING.
Así la instancia **sigue mapeada** después del corte y se pueden leer los
contadores bajo el mutex; `closing_release` destruiría las primitivas y
haría inválido el mapeo justo en el momento en que hay que afirmar. El
protocolo de cierre completo con el ejecutable real es el Issue #22.

### Pausas en 0

Los cuatro casos corren con `--mean-ms 0`. No es por ahorrar tiempo: medido
en la corrida de 4+4, con las pausas desactivadas los productores
acumularon 165 esperas en `not_full` y los consumidores ~115 en
`not_empty`, o sea que las dos esperas bloqueantes se ejercitan igual, con
la máxima contención posible sobre el buffer. La variedad de medias de
pausa es el eje del experimento (#24, D27), no de estas pruebas.

### Esperar el registro de los consumidores y no el de los productores

`run_start` espera a que los N consumidores estén registrados antes de
levantar los productores, y del lado productor **no espera registro**: un
consumidor no se retira hasta el cierre, así que su contador sube y no
baja, pero un productor puede cumplir su cuota y retirarse antes de que el
último se registre, y un `wait_for(active_producers == N)` se colgaría sin
que haya ningún error. Lo que se habría comprobado con esa espera lo cubre
igual la comparación de `enqueue_seq` contra `N × cuota`, que no tiene
carrera.

### Deadlock

El criterio "sin deadlock" se mide con plazos, no con la ausencia de
síntomas: `wait_exit` devuelve `-1` si el proceso no termina en 30 s y el
caso falla nombrando al que no avanzó (`"consumidor 1 terminó con -1 (no
terminó en el plazo: deadlock)"`); `alarm(180)` es la red por si el que se
cuelga es el propio proceso de prueba.

### Los hijos y los caminos de falla

Cada proceso lanzado queda anotado hasta que `wait_exit` lo cosecha, y lo
que quede se mata al salir. Hacía falta: si `run_join` se va **antes** del
corte a CLOSING (por ejemplo porque un productor terminó con código ≠ 0),
los consumidores siguen esperando datos que ya nadie va a insertar y
quedaban bloqueados para siempre —32 de ellos en el caso de estrés—
aunque la prueba ya hubiera terminado. `kill()` sobre un pid anotado no
puede caer en un proceso ajeno: un hijo que el padre no cosechó sigue
siendo zombie o vivo, así que el núcleo no puede reciclar su pid.

La misma limpieza corre desde un handler de SIGALRM, SIGINT, SIGTERM y
SIGPIPE, cuya acción por defecto termina el proceso **sin** pasar por
`atexit` y dejaba hijos bloqueados y segmentos en `/dev/shm`. Apareció al
correr la prueba con `| head`, que cierra la tubería. `kill()`,
`waitpid()`, `shm_unlink()` y `_exit()` son async-signal-safe, así que la
misma función sirve en los dos caminos.

### Que no pase sola: las mutaciones

Una prueba de integración que pasa sin ejercitar nada es peor que no
tenerla, así que cada afirmación se falsificó:

| Mutación | Lo que reportó |
|---|---|
| `expected + 1` en `run_join` | `fifo: 1000 aceptados y 0 productores activos; se esperaban 1001 y 0` |
| Borrar **una** fila `GET` de un CSV antes de verificar | `mensaje perdido: insertado y nunca extraido` + `cierre incompleto: CLOSING dice 4000, los logs dicen 3999` |
| `SIGSTOP` a un consumidor tras el corte | `consumidor 1 termino con -1 (no termino en el plazo: deadlock)` |
| Fallar antes del corte a CLOSING (estrés) | el caso falla **y** no quedan 32 consumidores huérfanos ni segmentos en `/dev/shm` |

La segunda es la que prueba el cableado completo de `./verify`: que las 9
rutas llegan, que el código de salida se honra y que el reporte se imprime
cuando hay violaciones.

### Detalles de implementación

- `run_verify` arma el comando en un buffer del tamaño justo porque el caso
  de estrés pasa **65** archivos, y lee la salida de `./verify` con `popen`
  **hasta EOF** aunque ya no la guarde: cortar al llenar el buffer dejaría
  a `verify` bloqueado escribiendo en una tubería llena y `pclose` esperaría
  para siempre.
- La salida de `verify` se imprime solo si hubo violaciones (limitadas a 20
  por `MAX_PRINTED`, D16), para que `make test` no se llene de ruido.
- `cut` está repetido de `test_blocking.c` en vez de vivir en `test_util.h`:
  ese header no incluye ningún header del proyecto a propósito (solo
  `stdbool`, `stddef` y `stdio`), y moverlo ahí le metería `memory.h` y
  `logger.h` a todas las pruebas, incluidas las de módulos que no tocan la
  instancia.
- Si un caso falla, sus CSV **quedan** en `build/` para poder mirarlos; solo
  se borran cuando el caso pasa.

### Costo

Las cuatro corridas tardan ~0.9 s (41 000 mensajes y 123 000 filas de log
verificadas) de los ~8.8 s de `make test` completo.

### Fuera de alcance

El cierre con el ejecutable `finisher` y las carreras de ciclo de vida son
el Issue #22; correr estos mismos cuatro casos bajo ASan y UBSan es el
Issue #23.

---

## D31 — Las dos mutaciones que sobrevivían al cierre, y el límite ante caídas

- **Fecha:** 2026-10-28
- **Issue:** [#50](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/50)
  (seguimiento de la revisión del PR #49, que aprobó el #17)
- **Quién decide:** Emmanuel · **Revisa:** Isaac

> Numeración: D30 es la entrada del Issue #20, en el PR #55. Esta entrada
> se escribió después y se mezcla después.

### Contexto

El código del cierre (#17) se aprobó sin pruebas que lo sostuvieran, para
no frenar la ruta crítica, con el acuerdo de hacerlas en este issue. Lo que
había que cubrir no era "¿funciona el cierre?" —eso ya lo mostraba el ciclo
completo con los cinco ejecutables— sino dos **mutaciones que las 6 pruebas
del finisher no distinguían**: el ciclo `while` de `closing_wait_all_left` y
la transición a `STATE_CLOSED`. Una prueba que no cambia de resultado cuando
el código se rompe no está probando esa línea.

### El ciclo `while` y no un `if`

`closing_wait_all_left` duerme en `all_left` mientras quede alguien
conectado además del finisher. Con **un** participante, cambiar el ciclo por
un `if` es indistinguible: hay un solo `signal` y después `refs` ya vale 1.

Hacen falta **dos retiradas en momentos distintos**, y lo que hace la prueba
es su **orden**, no su cantidad:

| Momento de la retirada | Con `while` | Con `if` |
|---|---|---|
| A a los 150 ms (el finisher ya duerme) | sigue esperando a B | **libera las primitivas** |
| B a los 500 ms | ALL_LEFT y liberación | B aborta en `pthread_mutex_lock` |

Los 150 ms del primero son para que el finisher **ya esté dormido** cuando
llegue ese `signal`. Si A se retirara antes del corte, `refs` valdría 2 al
entrar al ciclo y un `if` también acabaría esperando a B: la mutación
sobreviviría y la prueba no serviría de nada.

`tests/test_finisher.c::waits_for_every_participant` exige las dos cosas que
pide el criterio: que **los dos participantes salgan con 0** y que
`ALL_LEFT.t_ns >= max(retiradas)`, con las marcas de retirada llegando por
tubería desde cada participante. Los códigos de salida se comprueban
**antes** de leer las tuberías para que el diagnóstico sea el del criterio y
no un `read` fallido.

### La transición a `STATE_CLOSED`

No se puede observar desde el ejecutable: el finisher libera el nombre y
desmapea, así que cuando termina ya no hay segmento que mirar. La prueba
hace el cierre **en proceso**, con las tres llamadas en el orden de D10
(`closing_begin` → `closing_wait_all_left` → `closing_release`), y lee el
estado **antes de desmapear**:

- tras el corte, `STATE_CLOSING` leído bajo el mutex;
- tras `closing_release`, `STATE_CLOSED` leído **sin** mutex, porque ya no
  hay mutex. Es legítimo por lo mismo que el finisher puede leer ahí los
  agregados para el resumen (D10): la puerta está cerrada y este proceso es
  el único conectado, así que nadie más puede escribir el segmento.

No hace falta registrar a nadie: sin participantes activos ni mensajes
pendientes se cumple la segunda rama de la precondición del corte, y
`memory_create` no toma referencia de conexión (`refs == 0`, D9), así que
`closing_wait_all_left` retorna con el mutex tomado sin llegar a dormir.

### Resultado de las mutaciones

| Mutación en `src/participants.c` | 6 pruebas originales | La prueba nueva |
|---|---|---|
| `while` → `if` en `closing_wait_all_left` | 6/6 pasan | **FALLA**: `sync: pthread_mutex_lock: Invalid argument` y `wait_exit(b, 5) == 0` |
| quitar `inst->state = STATE_CLOSED` | 6/6 pasan | **FALLA**: `inst->state == STATE_CLOSED` |

**Trampa al comprobar una mutación:** `make` solo relinkea los ejecutables,
no los binarios de prueba. Correr `./build/test_finisher` después de tocar
`src/` mide el binario viejo y la mutación parece "sobrevivir": pasó en este
issue con la segunda mutación, que dio un falso negativo hasta pedir
`make build/test_finisher`. Es la misma advertencia de D25, ahora con un
segundo caso.

### Límite: un participante que muere sin retirarse cuelga el cierre

Medido con dos corridas que **solo** se diferencian en eso. En las dos
quedan mensajes pendientes y ningún productor activo, así que la
precondición del corte (D10) exige `active_consumers >= 1`:

| Corrida | Lo que hizo el finisher | Código |
|---|---|---|
| A (control): 2 pendientes, **ningún** consumidor | `rechazado: no hay consumidores registrados y queda trabajo pendiente`, inmediato | 1 |
| B: 4 pendientes y un consumidor **registrado** muerto con `SIGKILL` | `CLOSING: aceptados=4 extraidos=1 pendientes=3` y **no retorna** (5 s de plazo) | 124 |

Que B pase la precondición es la prueba de que el muerto **sigue contado**
como registrado; que no retorne es la prueba de que la espera no termina. El
segmento queda en CLOSING y el nombre tomado: un `creator` posterior con ese
nombre falla con `ya existe una instancia con ese nombre` (código 1).

**No es un defecto.** El enunciado no pide recuperación ante caídas, y
ponerle un plazo a la espera sería peor que el problema: el finisher
destruiría el mutex y las condvars con alguien todavía vivo, que es
exactamente el fallo que produce la mutación del `while` (D9 apoya la
liberación en que `refs == 1`, no en un reloj). Recuperarse de verdad
exigiría mutexes robustos (`PTHREAD_MUTEX_ROBUST` y `EOWNERDEAD`) y una
política de qué hacer con los mensajes del caído, que está fuera de alcance.

**Consecuencia práctica para el experimento (#24).** Si el script mata un
proceso por plazo y después lanza el finisher, la corrida se estanca.
`scripts/experimento.py` ya está armado para eso y conviene no deshacerlo:

- el finisher se lanza **solo** después de que los productores terminaron
  bien; si `wait_all` venció el plazo, mata a los procesos y la corrida se
  marca `invalida` sin llegar a lanzarlo;
- `kill_survivors` corre en el `finally` de cada corrida, después del
  finisher, y `cleanup_segment` libera el nombre a mano para que la corrida
  siguiente no choque con `MEM_EXISTS`;
- el finisher corre con `timeout=120`, así que en el peor caso (un
  participante que se cae por su cuenta mientras el cierre está en curso) la
  corrida se marca `invalida` en vez de colgar el experimento.

---

## D32 — Pruebas de validación y aislamiento; el log no se trunca a otro proceso

- **Fecha:** 2026-10-28
- **Issue:** [#19](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/19)
- **Quién decide:** Isaac · **Revisa:** Emmanuel
- **Numeración:** D32 porque D30 (Issue #20, PR #55) y D31 (Issue #50,
  PR #56) se abrieron antes.

### Contexto

El issue pide dos casos de la tabla de pruebas mínimas dentro de
`make test`: "Validación" (argumentos ausentes, capacidad 0, no
numéricos, fuera de rango, id duplicado, instancia inexistente, con error
claro y sin corromper otra ejecución) y "Aislamiento y reutilización" (dos
nombres sin interferencia, creación duplicada rechazada, recreación tras
el cierre). Al escribir el caso del id duplicado apareció una falla real.

### La falla

`csv_logger_open` abría con `fopen(path, "w")`, que trunca al abrir, y
productor, consumidor y finisher abren el log **antes** de conectarse y
registrarse (D23, para que los rechazos también queden en el CSV). Un
proceso con id duplicado y el mismo `--log` que uno en curso era
rechazado, pero antes le vaciaba el archivo al otro. Ese otro seguía
escribiendo en su posición anterior, y el archivo quedaba con un hueco de
bytes nulos. Reproducido en Docker: consumidor en curso con 3000 mensajes,
duplicado rechazado con el mismo log, y el CSV final tenía 360 273 bytes
nulos; `verify` encontró 2976 violaciones. Los mensajes se habían procesado
bien: lo que se destruía era la evidencia de la otra ejecución. Con pocos
mensajes no se veía, porque el consumidor aún no había vaciado su buffer
de `stdio`.

### Decisión

`csv_logger_open` abre **sin truncar** (`open` con `O_WRONLY | O_CREAT`),
toma un candado de escritura de todo el archivo con `fcntl(F_SETLK)` sin
esperar, y solo con el candado trunca (`ftruncate`) y escribe la cabecera.
Si otro proceso tiene el candado, devuelve `-EBUSY` sin modificar el
archivo y
el ejecutable termina con código 1 y el mensaje *"otro proceso en
ejecucion ya escribe en ese archivo"* (`csv_logger_open_str`). El candado
se suelta al cerrar el archivo o al morir el proceso, así que un proceso
que muere no deja el log bloqueado.

Alternativas descartadas:

- **Abrir el log después de registrarse:** pierde las filas de rechazo
  de D23, y no evita que dos procesos válidos (ids distintos) compartan el
  mismo log y lo intercalen. El candado cubre los dos casos.
- **`O_EXCL`:** impediría reusar la ruta en una corrida nueva, que es lo
  normal en el experimento y en el protocolo del README.

Los candados de `fcntl` son por proceso: dos aperturas del mismo archivo
dentro de un mismo proceso no se excluyen. Eso no pasa en los ejecutables
(cada uno abre un solo log).

**Solo archivos regulares.** Con `fstat` se mira qué se abrió, y el
candado y el truncado se aplican solo si es un archivo regular
(`S_ISREG`). Lo demás (`/dev/null`, un FIFO, una terminal) se usa tal
cual, igual que con `fopen("w")`: no tiene contenido que proteger, no se
puede truncar (`ftruncate` da `EINVAL` sobre un dispositivo) y `/dev/null`
es un solo archivo para toda la máquina, así que con candado un solo
proceso a la vez podría usar `--log /dev/null`. La primera versión de este
cambio lo rompía; lo encontró la revisión de Emmanuel, porque
`scripts/efecto_logs.py` (#25) usa `--log /dev/null` en 9 procesos a la
vez como línea base para medir el efecto de los logs.

**Qué deja un rechazo en el disco.** `open(..., O_CREAT)` crea el archivo
si no existía, antes de mirar el candado. Pero un rechazo por candado
implica que otro proceso ya tenía ese archivo abierto, o sea que existía:
lo que se garantiza es que su contenido no cambia. Los rechazos por
registro (id duplicado, instancia inexistente) sí escriben su CSV con la
fila de rechazo, a propósito (D23).

### Pruebas

- `tests/e2e_util.h`: lanza los ejecutables reales con `fork` + `execv`,
  con tiempo límite por proceso (`e2e_wait` mata al que no termina) y
  `alarm(120)` global, y captura `stderr` para revisar el mensaje.
- `tests/test_validation.c` (7 casos): argumentos ausentes en los cuatro
  ejecutables; capacidad 0, no numérica, con signo, con basura, vacía,
  fuera de rango y con overflow; nombres inválidos; opciones desconocidas,
  repetidas y ajenas al rol; números inválidos de productor y consumidor
  contra una instancia viva, comprobando que no se crea el log ni cambia
  la instancia; instancia inexistente; e id duplicado (consumidor activo,
  productor ya retirado y mismo log) sin cambiar la instancia, con la
  ejecución en curso terminando bien y aprobada por `verify`.
- `tests/test_isolation.c` (4 casos): dos instancias a la vez con los
  mismos ids, donde cerrar una no detiene a la otra y cada juego de logs
  pasa `verify`; creación duplicada rechazada sin tocar la existente; 8
  `creator` compitiendo por el mismo nombre, 5 rondas, siempre gana
  exactamente uno; y tres vidas seguidas del mismo nombre con distinta
  capacidad, reusando los mismos ids.
- `test_logger.c`: `open_busy_keeps_other_file`, con otro proceso
  sosteniendo el candado, y `open_dev_null_shared`, con dos procesos
  usando `/dev/null` a la vez.

Mutaciones detectadas (en Docker): volver a `fopen("w")` en el logger,
quitar `O_EXCL` del `shm_open` de `memory_create` (no del logger: ahí
`O_EXCL` es la alternativa descartada de arriba), aceptar capacidad 0, no
hacer `shm_unlink` al cerrar, aceptar el id duplicado, y considerar
duplicado solo a los activos.

Las pruebas que lanzan procesos (`e2e_util.h`) matan a los hijos que
queden vivos al salir, también si la salida es por `SIGALRM`, `SIGINT`,
`SIGTERM` o `SIGPIPE`, que no pasan por `atexit`. Es el mismo patrón que
`test_concurrency.c` (D30). `test_logger.c` tiene su propio `alarm`, y el
padre cierra los extremos de tubería que no usa, para que la muerte del
hijo le llegue como EOF y no como una espera eterna. La suite completa
pasa con ASan y UBSan (198 pruebas).

---

## D34 — Pruebas de cierre y carreras de ciclo de vida (`tests/test_closing.c`)

- **Fecha:** 2026-10-30
- **Issue:** [#22](https://github.com/ebarrantes07/SOA-P2-GrupoIE/issues/22)
- **Quién decide:** Isaac · **Revisa:** Emmanuel
- **Numeración:** D34 porque D33 queda reservada para el análisis del
  experimento (#25), que Emmanuel ya referencia en su rama.

### Contexto

`test_finisher.c` (#17, #50, D26, D31) prueba el protocolo de cierre con
participantes simulados dentro del proceso de prueba, que es lo que deja
apuntar a una línea concreta (el `while` de `closing_wait_all_left`, la
asignación de `STATE_CLOSED`). Lo que pide #22 es la otra mitad: los casos
de cierre de la tabla de pruebas mínimas con los ejecutables que corre el
usuario, `./creator`, `./producer`, `./consumer` y `./finisher`, y
`./verify` sobre todos los CSV.

### Casos

| Caso | Escenario | Qué se exige |
|---|---|---|
| Cierre durante actividad | capacidad 1 y 4; un consumidor lento y tres productores sin cuota; se corta con el buffer lleno y los tres productores registrados | finisher, productores y consumidor salen con 0; la fila `CLOSING` tiene pendientes > 0; `verify` aprueba (todo lo aceptado se extrajo, nada se insertó después del corte); nombre liberado |
| Cierre en vacío | tres consumidores dormidos en `not_empty` después de drenar | los tres terminan con 0 en menos de 2 s; `CLOSING` con 0 pendientes; `verify` aprueba |
| Sin trabajadores | `creator` y `finisher`, nada más | cierre completo (`ALL_LEFT`, `CLOSED`, resumen) y el nombre se puede volver a crear |
| Sin consumidor, productor activo | productor sin cuota bloqueado con el buffer lleno | el finisher sale con 1, sin resumen; la instancia sigue OPEN con los mismos contadores y el productor sigue vivo; al llegar un consumidor el cierre procede y `verify` aprueba todos los logs, el del rechazo incluido |
| Sin consumidor, datos pendientes | 3 mensajes y ningún participante | igual que el anterior, y los 3 se extraen después |
| Incorporaciones durante el cierre | un consumidor lento drena 64 pendientes (~1 s); mientras tanto llegan, cada 15 ms, productores, consumidores, finisher y creator con el mismo nombre | todos los tardíos salen con **1** (nunca por señal) con un mensaje del cierre; ninguno inserta ni extrae; ningún finisher tardío escribe resumen; al terminar, uno más encuentra el nombre libre; `verify` aprueba el conjunto, filas de rechazo incluidas |
| Finalizadores concurrentes | 4 finisher a la vez sobre una instancia con productor y consumidor activos, 10 rondas | gana exactamente uno y los otros tres salen con 1, sin resumen; los trabajadores salen con 0; una instancia recreada con el mismo nombre sigue existiendo después |

La regla de las carreras es la del issue: nadie usa un recurso destruido
ni lo destruye dos veces. Las dos cosas tienen síntoma observable. Tomar un
mutex destruido aborta el proceso por la política de D18, así que se exige
código de salida 1 y no "distinto de cero". Un segundo `shm_unlink`
tardío borraría la instancia recreada, así que se recrea y se comprueba que
sigue ahí.

### Cambios de apoyo

- `tests/e2e_util.h` (D32) suma `e2e_spawn_all`, que junta `stdout` y
  `stderr`, porque el finisher anuncia sus rechazos por `stdout`. También
  suma `e2e_peek` / `e2e_wait_for`, que leen el estado de la instancia en
  una sola región crítica (ahora también los usa `test_isolation.c`),
  `e2e_verify` para una cantidad variable de CSV, y `e2e_file_contains`,
  que no imprime nada cuando el texto no aparece.
- `e2e_path` borra un archivo viejo con esa ruta. Pedir dos veces la misma
  ruta borra lo que se escribió entre medio, así que cada ruta se pide una
  sola vez. Pasó mientras se escribían estas pruebas y quedó anotado en el
  propio header.

### Validación

En Docker: `make test` con 205 pruebas y 0 fallas, también con ASan y
UBSan, sin segmentos en `/dev/shm`; `test_closing` corrió 15 veces
seguidas sin fallas (~4 s cada vez), y un `SIGALRM` a mitad de la corrida no deja procesos ni segmentos. Mutaciones detectadas:

| Mutación | Casos que fallan |
|---|---|
| `pthread_cond_signal` en vez de `broadcast` al cortar | durante actividad, en vacío |
| cerrar sin exigir consumidor | los dos sin consumidor |
| no rechazar un segundo `closing_begin` | finalizadores concurrentes |
| el consumidor se va al ver CLOSING sin drenar | durante actividad, sin consumidor, incorporaciones, finalizadores |
| `sync_put` acepta después del corte | durante actividad, sin consumidor, finalizadores |
| `instance_attach` ignora la puerta cerrada | finalizadores concurrentes |

La última solo la detecta el caso de finalizadores. Sin la puerta, un
participante tardío que llega en CLOSING igual es rechazado al registrarse
(`REG_NOT_OPEN`). El daño aparece cuando la conexión llega después de
`closing_release`, con las primitivas ya destruidas, y eso es lo que
provocan los finisher perdedores de cada ronda.

