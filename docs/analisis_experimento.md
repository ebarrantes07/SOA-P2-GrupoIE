# Análisis del experimento

Resultados de las 45 corridas del experimento del Proyecto 2 (Issue #25). Los datos
están en `results/seleccionados/`: el manifiesto de las corridas, los 45 resúmenes
del finisher, los agregados y las tres figuras. Las decisiones metodológicas están
en `docs/decisiones_diseno.md` (D27 el experimento, D32 este análisis).

## 1. Qué se midió

| Parámetro | Valor |
|---|---|
| Capacidades | 1, 8, 64 |
| Escenarios (media de pausa productor/consumidor) | 2/8 ms, 5/5 ms, 8/2 ms |
| Repeticiones por combinación | 5 (45 corridas en total) |
| Procesos por corrida | 4 productores + 4 consumidores + creator + finisher |
| Mensajes | 200 por productor, 800 por corrida |
| Semilla base | 20261027, con semillas derivadas por proceso y corrida |

Las pausas son exponenciales con la media indicada, así que su desviación estándar
es **igual** a su media: fluctúan mucho, y esa fluctuación es protagonista de todo
lo que sigue. Las 45 corridas terminaron válidas: `verify` sin violaciones,
`invariant_ok = 1`, y 800 aceptados = 800 extraídos en cada una. El análisis se
niega a correr si alguna no cumple eso (`scripts/analisis.py`), porque promediar
una corrida con pérdidas daría números que no significan nada.

## 2. Entorno de medición

| Componente | Valor |
|---|---|
| Procesador | 12th Gen Intel Core i7-1260P, 16 CPU lógicas (lscpu: 8 núcleos × 2 hilos), 2496 MHz nominales |
| Memoria | 7.6 GiB |
| Sistema | Ubuntu 24.04.4 LTS sobre WSL2, kernel 6.18.33.2-microsoft-standard-WSL2, x86-64 |
| Compilador | GCC 13.3.0 (`-std=c17 -O0` implícito, sin `-O`), GNU Make 4.3 |
| Biblioteca C | glibc 2.39 |
| Reloj | `CLOCK_MONOTONIC` para todas las marcas de tiempo |

Tres cosas de este entorno afectan la lectura de los números y conviene decirlas
antes de los resultados:

- **Es un hipervisor, no hardware desnudo.** WSL2 presenta 16 CPU uniformes a
  2496 MHz. El i7-1260P es un diseño híbrido (núcleos P y E con distinta
  capacidad), pero el hipervisor no expone esa diferencia, así que no se puede
  fijar un proceso a un tipo de núcleo ni saber en cuál corrió. Un proceso que
  migra de un tipo a otro en medio de la corrida es una fuente de varianza que no
  se puede controlar ni medir desde dentro.
- **No se compila con optimización.** El Makefile no pasa `-O`, así que los
  números son de un binario sin optimizar. Es consistente entre corridas (todas
  usan el mismo binario), de modo que las comparaciones valen; los valores
  absolutos serían mejores con `-O2`.
- **Los 9 procesos de una corrida caben de sobra en 16 CPU**, así que ningún
  resultado está explicado por falta de procesador.

## 3. Resultados

Media ± desviación estándar entre las 5 repeticiones. La desviación es la muestral
(n−1). Tablas completas en `results/seleccionados/agregados.csv`.

### 3.1 Throughput

El techo teórico de cada escenario es la tasa del lado más lento: con 4 procesos
por lado y pausas de media `m`, cada lado mueve `4000/m` mensajes por segundo, y el
sistema no puede ir más rápido que el mínimo de los dos.

| Capacidad | 2/8 ms (techo 500) | 5/5 ms (techo 800) | 8/2 ms (techo 500) |
|---|---|---|---|
| 1 | 488.1 ± 14.7 — **98 %** | 611.4 ± 7.2 — **76 %** | 459.5 ± 32.1 — **92 %** |
| 8 | 492.4 ± 17.3 — **98 %** | 695.4 ± 29.4 — **87 %** | 477.0 ± 26.2 — **95 %** |
| 64 | 473.2 ± 15.7 — **95 %** | 721.5 ± 13.6 — **90 %** | 445.4 ± 12.5 — **89 %** |

Figura: `results/seleccionados/fig1_throughput.svg`.

### 3.2 Latencia de inserción a extracción

| Capacidad | 2/8 ms | 5/5 ms | 8/2 ms |
|---|---|---|---|
| 1 | 1.95 ± 0.06 | 0.77 ± 0.07 | 0.08 ± 0.01 |
| 8 | 15.99 ± 0.58 | 4.77 ± 0.82 | 0.08 ± 0.01 |
| 64 | **128.05 ± 4.74** | 24.62 ± 23.81 | 0.06 ± 0.01 |

Medias en ms; el percentil 95 y el máximo están en los agregados y en la figura
`fig2_latencia.svg`, con eje logarítmico porque los valores cubren de 0.06 a 160 ms.

### 3.3 Dónde se espera

Espera acumulada de cada rol como porcentaje de su propio tiempo de vida, y
cantidad de bloqueos por corrida (de 200 operaciones por proceso).

| Capacidad | 2/8 ms | 5/5 ms | 8/2 ms |
|---|---|---|---|
| 1 | prod **73.3 %** (192 bloqueos) · cons 1.7 % (7) | prod 15.8 % (83) · cons 20.9 % (87) | prod 0.1 % (4) · cons **75.3 %** (197) |
| 8 | prod **73.4 %** (190) · cons 0.5 % (1) | prod 6.1 % (26) · cons 13.3 % (46) | prod 0.0 % (0) · cons **74.8 %** (196) |
| 64 | prod **71.2 %** (172) · cons 0.3 % (1) | prod 0.6 % (2) · cons 9.0 % (30) | prod 0.0 % (0) · cons **77.1 %** (197) |

Figura: `results/seleccionados/fig3_esperas.svg`.

## 4. Lectura: ¿aumentar la capacidad mejora, acumula o desplaza?

**Las tres cosas, y cuál de ellas ocurre lo decide el balance entre los lados.**

### Cuando un lado es el cuello de botella: solo acumula

En 2/8 ms el consumidor es el lado lento, y el sistema ya corre al **98 % de su
techo con capacidad 1**. No hay nada que mejorar: de 1 a 64 casillas el throughput
se queda en ~490 msgs/s, y la espera del productor no se mueve (73.3 % → 71.2 % de
su vida). Lo único que cambia es **cuántos mensajes hay esperando**, y eso se paga
en latencia: **1.95 → 15.99 → 128.05 ms**, un factor de 66 entre capacidad 1 y 64.

La razón se ve en los bloqueos: el productor se bloquea en **190 de 200**
inserciones, o sea que el buffer está lleno casi todo el tiempo, y el consumidor se
bloquea **1 vez por corrida**. El cuello de botella nunca se queda sin trabajo, así
que corre a su propia tasa y es indiferente a cuántas casillas haya delante. Las
casillas extra solo sirven para que la cola sea más larga.

El caso simétrico, 8/2 ms, es el mismo fenómeno del otro lado: el consumidor espera
el 75 % de su vida, el buffer está casi siempre vacío y la latencia es mínima
(0.06–0.08 ms) porque un mensaje insertado se extrae de inmediato. Acá la capacidad
no acumula nada: no hay con qué llenar el buffer.

### Cuando los dos lados están balanceados: mejora, a cambio de latencia

En 5/5 ms ninguno de los dos es el cuello de botella, y es el único escenario donde
la capacidad **sí** mejora el throughput: 611 → 695 → **721 msgs/s** (+18 %), del
76 % al 90 % del techo.

El mecanismo está en la tabla de esperas: con capacidad 1, **los dos lados pierden
tiempo a la vez** —el productor se bloquea 83 veces y el consumidor 87, el 16 % y
el 21 % de su vida— sin que ninguno esté saturado. Esa es la firma del
acoplamiento: como las pausas fluctúan tanto como su media, cada fluctuación de un
lado frena al otro, y ese tiempo no se recupera. Con capacidad 64 el productor
deja de bloquearse (0.6 % de su vida, 2 bloqueos por corrida) y queda solo el 9 %
del consumidor, que es la fluctuación que ni 64 casillas alcanzan a absorber.

**El buffer no acelera a nadie: absorbe varianza.** Y solo hay varianza que
absorber cuando ninguno de los dos lados es el cuello de botella. Por eso la misma
capacidad que no aporta nada en 2/8 recupera la mitad del déficit en 5/5.

El precio es latencia: en 5/5 la media pasa de 0.77 a 24.62 ms. Cambiar 18 % de
throughput por 32× de latencia es un intercambio, no una mejora gratuita, y qué
lado conviene depende de para qué sirva el sistema.

### Y desplaza la espera entre roles

Comparando las columnas de la tabla 3.3, la espera **cambia de dueño** según el
escenario: en 2/8 la carga la lleva el productor (73 % de su vida en `not_full`);
en 8/2 la lleva el consumidor (75 % en `not_empty`); en 5/5 está repartida y la
capacidad la va **drenando** de los dos lados a la vez. La capacidad no reparte la
espera entre roles —eso lo hace el escenario— pero sí reduce la parte de esa espera
que es evitable.

## 5. Efecto de los logs sobre la medición

Cada corrida escribe ~155 KiB de CSV repartidos en 9 archivos, uno por proceso
(4 productores, 4 consumidores y el finisher): 3 filas por mensaje (`PUT`, `GET`,
`PROCESSED`), unos 200 bytes por mensaje. Para saber cuánto distorsiona eso
lo medido, `scripts/efecto_logs.py` corre el mismo escenario con los CSV en un
archivo y con los CSV a `/dev/null`, 7 repeticiones por celda
(`results/seleccionados/efecto_logs.csv`):

| Régimen | Con logs | A `/dev/null` | Diferencia |
|---|---|---|---|
| Pausas 5/5 ms (el del experimento) | 679.5 ± 47.1 | 671.7 ± 25.3 | −1.1 %, **0.4 errores estándar: no medible** |
| Sin pausas (`--mean-ms 0`, 5000 msgs/productor) | 106 213 ± 5 079 | 112 595 ± 3 798 | +6.0 %, **2.7 errores estándar** |

**En el régimen en que se corrió el experimento, la instrumentación no contamina
los resultados:** la pausa simulada de 5 ms por mensaje domina por tres órdenes de
magnitud sobre escribir 206 bytes. El costo existe y es medible solo cuando no hay
pausas: ahí vale **0.53 µs de tiempo de pared por mensaje** (9.415 → 8.881 µs por
mensaje), un 6 % de la tasa máxima.

Dos razones de diseño ayudan a que el costo sea tan bajo, y las dos son decisiones
anteriores a esta medición: la escritura ocurre **fuera de la región crítica** (D3,
D12: bajo el mutex solo se copian los campos del evento), así que un `write` lento
no serializa el buffer; y cada proceso escribe **su propio** archivo, así que no hay
contención entre ellos por un archivo común.

Lo que este A/B **no** mide: con `/dev/null` el logger sigue formateando cada fila
con `snprintf` y sigue capturando los campos bajo el mutex. La comparación aísla la
escritura al archivo, no el formateo. Medir el formateo pediría una compilación sin
instrumentación, que no está en el alcance del proyecto.

## 6. Qué no se puede concluir de estos datos

- **El percentil 95 está cuantizado.** Lo aproxima un histograma con error ≤ 6.25 %
  (D11), así que valores vecinos caen en el mismo cubo: en 2/8 con capacidad 64 las
  5 repeticiones dieron **exactamente** 159.38 ms (desviación 0.00), que es un
  artefacto del cubo, no una medición perfectamente repetible.
- **Con capacidad grande y lados balanceados, la latencia es inestable.** En 5/5 con
  capacidad 64 las 5 repeticiones dieron 7.1, 7.7, 12.2, 33.6 y 62.5 ms de latencia
  media —un factor de 9— mientras el throughput se mantuvo entre 710 y 744. Cuánta
  cola se acumula depende de la suerte de las pausas: la capacidad **permite** la
  cola, no la obliga. Por eso esa celda tiene una desviación casi igual a su media,
  y por eso no conviene citar su latencia como un valor característico.
- **El `active_interval` incluye el drenaje del cierre**, porque termina en la
  última extracción. En 2/8 la duración del cierre pasa de 3.1 ms (capacidad 1) a
  **120.4 ms** (capacidad 64) —los ~64 mensajes pendientes a 8 ms repartidos entre
  4 consumidores— y el intervalo activo sube 66 ms en el mismo salto. Esa es la
  explicación más probable de la bajada de throughput de 492 a 473 en esa columna:
  la cola acumulada hay que pagarla al cerrar. La bajada es de ~1.8 errores
  estándar, así que lo correcto es decir que **no mejora**, no que empeora.
- **La espera del consumidor incluye su espera final hasta el cierre** (declarado en
  la nota de la fila `wait_data`, D28). En 8/2, donde el consumidor termina su
  trabajo antes, parte de ese 75 % es tiempo esperando el corte, no hambre de datos.
- **5 repeticiones y un solo entorno.** Las desviaciones de la tabla son de este
  equipo, bajo un hipervisor, con otras cargas del sistema compitiendo. Son
  suficientes para separar efectos grandes (el +18 % de 5/5, el factor 66 de
  latencia) y no para discutir diferencias de pocos por ciento.

## 7. Cómo reproducir

```bash
make                                    # los cinco ejecutables
python3 scripts/experimento.py          # las 45 corridas (~70 s)
python3 scripts/analisis.py --archivar  # agregados + copia de los resúmenes
python3 scripts/graficas.py             # las tres figuras SVG
python3 scripts/efecto_logs.py          # el A/B de los logs (~20 s)
```

El análisis también se repite **sin volver a correr el experimento**, a partir de
los resúmenes versionados en el repositorio:

```bash
python3 scripts/analisis.py --results results/seleccionados
```

Las semillas se derivan de `--base-seed`, así que repetir el experimento con la
misma base reproduce las mismas secuencias de pausas (D27).
