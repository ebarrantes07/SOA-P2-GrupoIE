# Decisiones de diseño

Bitácora viva de decisiones de diseño del Proyecto 2 (MC6004 SOA). Cada
entrada queda numerada (D1, D2…) y no se borra aunque una decisión posterior
la reemplace — en ese caso la entrada nueva dice explícitamente a cuál
reemplaza y por qué. El objetivo es que cualquiera (incluido el profesor)
pueda reconstruir por qué el código quedó como quedó sin tener que preguntar.

---

## D1 — Layout del segmento de memoria compartida

- **Fecha:** 2026-10-14
- **Issue:** [#2](../../issues/2) (Diseño A), slice 1
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

Dos consecuencias que vale la pena tener claras para la defensa:

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
y trazar a mano en la defensa.

---

## D2 — Buffer circular puro (`include/buffer.h`)

- **Fecha:** 2026-10-14
- **Issue:** [#2](../../issues/2) (Diseño A), slice 2
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
- **Issue:** [#2](../../issues/2) (Diseño A), slice 3
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
