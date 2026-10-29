# SOA-P2-GrupoIE

Proyecto 2 de MC6004 Sistemas Operativos Avanzados (II semestre 2026): sistema de múltiples
productores y consumidores, como procesos independientes, que intercambian mensajes mediante
un buffer circular acotado en memoria compartida POSIX, con cierre ordenado y medición
experimental.

## Integrantes

| Nombre | Carné | Usuario de GitHub |
|---|---|---|
| Emmanuel Barrantes Vargas | 200012366 | [ebarrantes07](https://github.com/ebarrantes07) |
| Isaac Moreno Fuentes | 2018119181 | [ifmoreno-sp](https://github.com/ifmoreno-sp) |

## Entorno

**Objetivo:** GNU/Linux de 64 bits, GCC y GNU make, C17 con APIs POSIX (memoria compartida
`shm_open`/`mmap` y primitivas de sincronización configuradas para compartirse entre procesos).

**Probado en:**

| Componente | Versión |
|---|---|
| Sistema | Ubuntu 24.04.4 LTS, x86-64 (contenedor Docker) |
| Compilador | GCC 13.3.0 |
| make | GNU Make 4.3 |
| Biblioteca C | glibc 2.39 |

`make test`, las corridas con ASan/UBSan y el experimento se validaron en ese entorno, compilando
desde una copia limpia del repositorio. El desarrollo se hizo además en macOS (Apple clang), donde
el código compila, pero la suite completa necesita Linux: algunas pruebas usan `/proc` y
`MAP_ANONYMOUS`, y macOS no implementa `pthread_condattr_setclock` (ver D13 y D20).

## Compilación

```bash
make          # compila los ejecutables: creator, producer, consumer, finisher y verify
make test     # ejecuta las pruebas automatizadas (código ≠ 0 ante un incumplimiento)
make clean    # elimina objetos y ejecutables
```

Flags de compilación:

```
-std=c17 -Wall -Wextra -Wpedantic -Werror -pthread
```

Además se define `_POSIX_C_SOURCE=200809L` como macro de características y se enlaza con
`-lm` (pausas exponenciales) y, en Linux, `-lrt` (`shm_open` en glibc antiguas).

## Estructura del repositorio

| Carpeta | Contenido |
|---|---|
| `src/` | Código fuente; cada ejecutable tiene su `main` en `src/<ejecutable>.c` |
| `include/` | Headers de los módulos |
| `tests/` | Pruebas automatizadas de `make test` |
| `scripts/` | Scripts del experimento, verificación de CSV y gráficas |
| `results/` | Resultados; solo se versiona `results/seleccionados/` |
| `docs/` | Bitácora de decisiones (`decisiones_diseno.md`) e informe |

## Material reutilizado

El generador pseudoaleatorio `xorshift32` de `src/rng.c` (estado de 32 bits y la secuencia
`x ^= x << 13; x ^= x >> 17; x ^= x << 5;`) se reutiliza tal cual del Proyecto 1 del mismo
grupo, [SOA-P1-GrupoIE](https://github.com/ifmoreno-sp/SOA-P1-GrupoIE) (`src/rng.c`). Lo
nuevo en este proyecto es el muestreo exponencial por transformada inversa
(`rng_uniform01` y `rng_exponential_ms`), que el Proyecto 1 no tenía.

**Licencia:** el Proyecto 1 es trabajo propio de los mismos integrantes, así que su código se
reutiliza sin restricciones de terceros. El checksum del payload usa el algoritmo FNV-1a
(Fowler–Noll–Vo), que sus autores publicaron en el dominio público; se implementó desde su
especificación, sin copiar código de terceros. Los scripts de `scripts/` usan solo la biblioteca
estándar de Python.

El resto del código es original de este proyecto.

## Uso

### Protocolo de ejecución y cierre

Cada ejecutable es un proceso independiente; se lanzan en terminales separadas o desde un script.

```bash
./creator  --name /soa_p2_g1 --capacity 8                                                   # 1
./consumer --name /soa_p2_g1 --id 1 --mean-ms 10 --seed 11 --log results/c1.csv &          # 2
./producer --name /soa_p2_g1 --id 1 --mean-ms 5 --seed 21 --messages 1000 --log results/p1.csv  # 3
./finisher --name /soa_p2_g1 --summary results/resumen.csv --log results/fin.csv          # 4
./verify   results/p1.csv results/c1.csv results/fin.csv                                   # 5
```

1. **`creator`** crea la instancia y termina cuando ya acepta participantes (estado OPEN).
2. **Consumidores** y **productores** se conectan por el nombre y se registran con un `--id` único
   en su rol. Pueden entrar en cualquier orden mientras la instancia esté OPEN. Un productor
   espera bloqueado con el buffer lleno; un consumidor, con el buffer vacío.
3. Los productores se retiran solos al cumplir su cuota (`--messages`). Sin cuota siguen hasta
   el cierre.
4. **`finisher`** conduce el cierre:
   - exige al menos un consumidor registrado, salvo que no queden productores activos ni
     mensajes pendientes; si no, rechaza con código 1 y la instancia sigue OPEN;
   - pasa a **CLOSING**, que es el punto de corte: desde ahí no se aceptan inserciones ni
     participantes nuevos, y despierta a todos los que esperaban;
   - los productores bloqueados o en pausa se retiran sin insertar nada más; los consumidores
     **drenan todo lo aceptado**, terminan de procesar lo que retiraron y se retiran;
   - cuando ya no queda nadie conectado, pasa a **CLOSED**, destruye las primitivas, elimina
     el nombre (que queda libre para crear otra instancia) y escribe el resumen.
5. **`verify`** cruza los CSV y confirma que no hubo pérdidas, duplicados, contenido alterado
   ni violaciones de FIFO, y que el cierre cuadra.

Solo puede haber un cierre en curso: un segundo `finisher` se rechaza con código 1. No se usan
señales de terminación como mecanismo normal de cierre.

### creator

```bash
./creator --name /soa_p2_g1 --capacity 8
```

Crea e inicializa la instancia y termina con código 0 cuando ya acepta participantes. El
nombre es `/` seguido de 1 a 30 caracteres `[A-Za-z0-9_-]` y la capacidad va de 1 a 1024.
Si ya existe una instancia con ese nombre, la rechaza sin modificarla (código 1).

### producer

```bash
./producer --name /soa_p2_g1 --id 1 --mean-ms 5 --seed 7 --log results/p1.csv
./producer --name /soa_p2_g1 --id 1 --mean-ms 5 --seed 7 --messages 1000 --log results/p1.csv
```

Se registra y repite: pausa exponencial de media `--mean-ms`, arma un mensaje con payload
verificable e inserta (esperando bloqueado si el buffer está lleno). Con `--messages` se retira al
aceptar esa cantidad; sin la opción sigue hasta el cierre. Al notar que la instancia se está
cerrando se retira sin insertar nada más, incluso si no alcanzó su cuota, y termina con código 0.
Cada evento queda en su CSV (formato en `docs/decisiones_diseno.md`, D12).

### consumer

```bash
./consumer --name /soa_p2_g1 --id 1 --mean-ms 10 --seed 11 --log results/c1.csv
```

Se registra, extrae mensajes en orden FIFO (esperando bloqueado si el buffer está vacío), verifica
el contenido de cada uno y simula su procesamiento con pausas exponenciales de media `--mean-ms`.
Termina solo cuando la instancia está cerrándose y ya no quedan mensajes, después de procesar todo
lo que retiró. Cada evento queda en su CSV (formato en `docs/decisiones_diseno.md`, D12).

### finisher

```bash
./finisher --name /soa_p2_g1 --summary results/resumen.csv
./finisher --name /soa_p2_g1 --summary results/resumen.csv --log results/fin.csv
```

Conduce el cierre ordenado: corta la instancia a CLOSING (desde ahí no se aceptan inserciones ni
participantes nuevos), espera a que todos los participantes se retiren, libera los recursos y el
nombre (que queda libre para crear otra instancia) y escribe el resumen. Si no hay consumidores y
todavía queda trabajo, o si ya hay otro cierre en curso, rechaza el cierre sin tocar la instancia
y termina con código 1. Los eventos se muestran en la consola y, con `--log`, también en su CSV.

### Opciones

| Opción | creator | producer | consumer | finisher | Valor |
|---|---|---|---|---|---|
| `--name` | ✓ | ✓ | ✓ | ✓ | `/` + 1 a 30 caracteres `[A-Za-z0-9_-]` |
| `--capacity` | ✓ | | | | entero de 1 a 1024 |
| `--id` | | ✓ | ✓ | | entero de 1 a 4294967295, único por rol |
| `--mean-ms` | | ✓ | ✓ | | número de 0 a 1000 (p. ej. `5` o `2.5`); `0` desactiva las pausas |
| `--seed` | | ✓ | ✓ | | entero de 1 a 4294967295 |
| `--messages` | | opcional | | | cuota positiva; sin ella el productor sigue hasta el cierre |
| `--log` | | ✓ | ✓ | opcional | ruta del CSV del proceso |
| `--summary` | | | | ✓ | ruta del CSV de resumen |

✓ = obligatoria. Cualquier argumento inválido, faltante, repetido o desconocido termina con un
mensaje en `stderr` y código 1. `--help` muestra el uso de cada ejecutable.

### verify

```bash
./verify results/p*.csv results/c*.csv results/fin.csv
```

Lee los CSV de una corrida y termina con 0 si no hay violaciones, 1 si encuentra alguna (pérdida,
duplicado, mensaje fantasma, identidad repetida, contenido alterado, orden FIFO, inserción después
del corte o cierre inconsistente, cada una con su identidad) y 2 si no pudo leer una entrada.
Detalles en D16.

## Formatos CSV

### Log de cada proceso (`--log`)

Un archivo por proceso, con cabecera y las mismas 13 columnas para productor, consumidor y
finisher. Si el archivo ya existe se sobrescribe, salvo que otro proceso en ejecución lo esté
escribiendo: entonces el nuevo proceso termina con código 1 sin tocarlo (D32). Una columna que no aplica al evento queda vacía. Todas las marcas de tiempo son de
`CLOCK_MONOTONIC`, en nanosegundos.

```
role,id,event,t_ns,producer_id,sequence,enqueue_seq,dequeue_seq,enqueue_time_ns,occupancy,checksum,payload_ok,detail
```

| Evento | Rol | Columnas que lleva |
|---|---|---|
| `JOIN` / `JOIN_REJECTED` | productor, consumidor | `detail` (pid, o el motivo del rechazo) |
| `PUT` | productor | `producer_id, sequence, enqueue_seq, enqueue_time_ns, occupancy, checksum` |
| `PUT_REJECTED` | productor | `producer_id, sequence, detail = closing` |
| `GET` | consumidor | `producer_id, sequence, enqueue_seq, dequeue_seq, enqueue_time_ns, occupancy, checksum, payload_ok` |
| `PROCESSED` | consumidor | `producer_id, sequence, dequeue_seq` |
| `DRAINED` | consumidor | `occupancy` |
| `LEAVE` | productor, consumidor | `detail` (`quota_done`, `closing` o `drained`) |
| `CLOSE_REQUEST`, `ALL_LEFT` | finisher | — |
| `CLOSE_REJECTED` | finisher | `detail` (`no_consumer` o `already_closing`) |
| `CLOSING` | finisher | `enqueue_seq` (aceptados), `dequeue_seq` (extraídos), `occupancy` (pendientes) en el corte |
| `CLOSED` | finisher | `detail = released` |

`occupancy`, `enqueue_seq`, `dequeue_seq` y `t_ns` de `PUT`/`GET` se capturan dentro de la
operación protegida; la escritura al archivo ocurre después de soltar el mutex. Formato completo
en D12 y D17.

### Resumen del finisher (`--summary`)

Una métrica por fila:

```
metric,role,id,value,unit,note
```

Filas de la instancia (`role = instance`): `accepted`, `extracted`, `pending`, `rejected_puts`,
`invariant_ok`, `latency_mean`, `latency_p95` (aproximada por histograma), `latency_max`,
`throughput`, `active_interval`, `close_duration`, `producers` y `consumers`. Filas por
participante: `ops`, `wait_space` o `wait_data`, `wait_count`, `lock_acquire` y `lifetime`. La
columna `note` declara cuándo una medición combina varias causas. Formato completo en D11 y D28.

## Experimento

```bash
make                                    # los cinco ejecutables primero
python3 scripts/experimento.py          # las 45 corridas (~70 s)
python3 scripts/experimento.py --quick  # 3 corridas, para probar el script
```

Corre las 45 combinaciones que pide el enunciado (capacidades 1/8/64 × medias
productor/consumidor 2/8, 8/2, 5/5 × 5 repeticiones) con 4 productores y 4 consumidores de 200
mensajes cada productor. Cada corrida deja sus CSV en `results/<combinación>_rep<N>/` y una fila
en `results/manifiesto.csv` con sus parámetros, sus semillas, los códigos de salida y el
resultado del verificador. Termina con código 0 solo si las 45 corridas quedaron válidas.

Las semillas se derivan de `--base-seed`, así que repetir el experimento con la misma base
reproduce las mismas secuencias de pausas. Detalles en `docs/decisiones_diseno.md` (D27).

## Versión entregada

_Pendiente: URL, etiqueta `p2-entrega` y hash completo del commit._
