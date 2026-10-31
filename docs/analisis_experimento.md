# Análisis del experimento

Resultados de las 45 corridas del experimento del Proyecto 2 (Issue #25). Los datos
están en `results/seleccionados/`: el manifiesto de las corridas, los 45 resúmenes
del finisher, los agregados y las tres figuras. El modelo de referencia con el que se
separa el régimen de la implementación (sección 5) es `scripts/modelo.py`. Las
decisiones metodológicas están en `docs/decisiones_diseno.md` (D27 el experimento,
D33 este análisis).

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

**El techo no es alcanzable cuando lo fija el lado con cuota.** `4000/m` supone que
el lado lento no para nunca, y en 8/2 ese lado es el productor, que tiene cuota fija
(`--messages 200`): la corrida no puede terminar antes de que **el más lento de los
cuatro** ponga sus 200 mensajes. Con 200 pausas exponenciales la media muestral de
cada productor se desvía 1/√200 = 7.1 %, y el máximo de cuatro cae ~1.03
desviaciones por encima, así que el intervalo se estira ~7.3 %:

    techo realista = (4000/m) / (1 + 1.03/√N) = 500 / 1.073 ≈ 466 msgs/s

Lo medido (459, 477, 445) lo rodea, y el modelo de la sección 5 llega al mismo 93 %
**sin mutex ni CSV**. Así que el 5-11 % que le falta a esa columna no es costo de
sincronización ni ineficiencia: es el estadístico de orden de la cuota. Del lado del
consumo no aparece porque los consumidores no tienen cuota — el que saca pausas
cortas procesa más mensajes y compensa al lento, de modo que 2/8 sí llega a su techo.
El mismo límite acota al 5/5, donde el productor también es quien tiene cuota.

### 3.2 Latencia de inserción a extracción

| Capacidad | 2/8 ms | 5/5 ms | 8/2 ms |
|---|---|---|---|
| 1 | 1.95 ± 0.06 | 0.77 ± 0.07 | 0.08 ± 0.01 |
| 8 | 15.99 ± 0.58 | 4.77 ± 0.82 | 0.08 ± 0.01 |
| 64 | **128.05 ± 4.74** | 24.62 ± 23.81 | 0.06 ± 0.01 |

Medias en ms. El **percentil 95**, recalculado exacto desde los CSV de los
consumidores como pide D11 (`analisis.py`, métrica `latency_p95_exact`):

| Capacidad | 2/8 ms | 5/5 ms | 8/2 ms |
|---|---|---|---|
| 1 | 5.95 ± 0.39 | 3.17 ± 0.27 | 0.17 ± 0.01 |
| 8 | 26.55 ± 0.99 | 13.41 ± 1.44 | 0.18 ± 0.02 |
| 64 | **160.52 ± 5.11** | 46.83 ± 33.26 | 0.13 ± 0.03 |

Y el **máximo**:

| Capacidad | 2/8 ms | 5/5 ms | 8/2 ms |
|---|---|---|---|
| 1 | 14.98 ± 2.78 | 8.38 ± 1.54 | 1.66 ± 0.49 |
| 8 | 38.50 ± 1.92 | 21.83 ± 4.41 | 1.47 ± 0.40 |
| 64 | 177.19 ± 9.60 | 56.73 ± 38.34 | 1.69 ± 0.93 |

La media y el p95 se grafican en `fig2_latencia.svg`, con eje logarítmico porque los
valores cubren de 0.06 a 160 ms. La figura dibuja el p95 aproximado del resumen, que
difiere del exacto en menos del 1.5 % y es indistinguible en ese eje.

**En 2/8 la latencia no es una medición, es una división.** La ley de Little
(`ocupación = throughput × latencia`) permite recuperar cuán lleno estuvo el buffer,
y en esa columna estuvo lleno casi todo el tiempo:

| Celda | Ocupación media | % de la capacidad |
|---|---|---|
| 2/8 cap 1 | 0.95 msgs | **95.4 %** |
| 2/8 cap 8 | 7.87 msgs | **98.4 %** |
| 2/8 cap 64 | 60.59 msgs | **94.7 %** |
| 5/5 cap 64 | 17.76 msgs | 27.8 % |
| 8/2 cap 64 | 0.03 msgs | 0.04 % |

Con el buffer lleno, un mensaje tarda en cruzarlo `capacidad / tasa de drenaje`. Eso
predice 2.05, 16.25 y 135.26 ms contra 1.95, 15.99 y 128.05 medidos (error ≤ 5.6 %):
**en régimen saturado, elegir la capacidad es elegir la latencia**, no influir sobre
ella. En 5/5 el buffer está al 28-47 %, con holgura, y ahí la latencia que sube es el
precio del amortiguamiento que gana throughput. En 8/2 está vacío y la columna mide
otra cosa (sección 5).

**La cola larga vuelve la latencia más predecible**, que es el lado del intercambio
que las medias esconden:

| Celda | p95/media | máx/media |
|---|---|---|
| 2/8 cap 1 | 3.03 | 7.66 |
| 2/8 cap 8 | 1.64 | 2.41 |
| 2/8 cap 64 | **1.24** | **1.38** |

Con capacidad 1 el peor mensaje tarda 7.7× la media; con 64, apenas 1.38×. Para un
compromiso de percentil —y no de promedio— peor media con menos dispersión puede ser
el lado conveniente del intercambio.

### 3.3 Dónde se espera

Espera acumulada de cada rol como porcentaje de su propio tiempo de vida, y
cantidad de bloqueos por corrida (de 200 operaciones por proceso).

| Capacidad | 2/8 ms | 5/5 ms | 8/2 ms |
|---|---|---|---|
| 1 | prod **73.3 %** (192 bloqueos) · cons 1.7 % (7) | prod 15.8 % (83) · cons 20.9 % (87) | prod 0.1 % (4) · cons **75.3 %** (197) |
| 8 | prod **73.4 %** (190) · cons 0.5 % (1) | prod 6.1 % (26) · cons 13.3 % (46) | prod 0.0 % (0) · cons **74.8 %** (196) |
| 64 | prod **71.2 %** (172) · cons 0.3 % (1) | prod 0.6 % (2) · cons 9.0 % (30) | prod 0.0 % (0) · cons **77.1 %** (197) |

Figura: `results/seleccionados/fig3_esperas.svg`.

**Las barras del 71-77 % son el cociente de las pausas, no una propiedad del
diseño.** El lado rápido solo puede trabajar a la tasa que le llega, así que su ciclo
de trabajo está forzado y su ocio sale en forma cerrada, `1 − m_rápido/m_lento`:

| Escenario | Predicción | Medido (cap 1 / 8 / 64) |
|---|---|---|
| 2/8, productor | `1 − 2/8` = **75.0 %** | 73.3 / 73.4 / 71.2 % |
| 8/2, consumidor | `1 − 2/8` = **75.0 %** | 75.3 / 74.8 / 77.1 % |

Con un escenario 2/16 esa barra estaría en 87.5 % con el mismo código y la misma
capacidad, y por eso es inmune a la capacidad: el cociente de las medias no depende
de cuántas casillas haya. El corolario es lo que vuelve informativa la columna del
medio: para 5/5 la predicción es `1 − 5/5 = 0 %`, así que **todo** lo que se mide ahí
—15.8 % y 20.9 % con capacidad 1— es pérdida por acoplamiento, y es exactamente la
parte que la capacidad drena. De las nueve celdas de la figura, seis están
determinadas por los parámetros del escenario; la información sobre el buffer vive en
la columna balanceada.

### 3.4 Costo por operación: el lado de la inserción

La sección 3.2 mide la residencia de un mensaje en el buffer (`enqueue → dequeue`),
que es una propiedad del *mensaje*. Lo que le cuesta a un **productor** cada
inserción es otra cosa, y sale de dividir su espera acumulada entre sus
operaciones. Se promedia por participante y después entre repeticiones: cada
proceso tiene su propio contador de operaciones, así que dividir los totales
pesaría más al que más trabajo hizo.

**Espera del productor por inserción [ms]** y **porcentaje de inserciones que se
bloquean**:

| Capacidad | 2/8 ms | 5/5 ms | 8/2 ms |
|---|---|---|---|
| 1 | 5.905 ± 0.194 — 96.0 % | 0.980 ± 0.131 — 41.3 % | 0.005 ± 0.001 — 2.0 % |
| 8 | 5.809 ± 0.364 — 95.1 % | 0.331 ± 0.129 — 13.2 % | 0.000 — 0.0 % |
| 64 | 5.437 ± 0.293 — 86.2 % | 0.032 ± 0.071 — **1.2 %** | 0.000 — 0.0 % |

**Duración de un bloqueo del productor cuando ocurre [ms]**:

| Capacidad | 2/8 ms | 5/5 ms | 8/2 ms |
|---|---|---|---|
| 1 | 6.153 ± 0.227 | 2.371 ± 0.105 | 0.284 ± 0.149 |
| 8 | 6.109 ± 0.333 | 2.488 ± 0.479 | 0.000 |
| 64 | **6.307 ± 0.312** | 0.559 ± 1.249 | 0.000 |

Tres lecturas, y la primera es la que no se ve en la 3.2:

1. **En 2/8, insertar le cuesta al productor ~5.9 ms, tres veces su propia pausa de
   2 ms**, y la capacidad no lo baja (5.905 → 5.437 ms de capacidad 1 a 64). Visto
   por operación, el productor no está produciendo: está esperando.
2. **La duración de cada bloqueo no depende de la capacidad**: ~6.2 ms en las tres
   capacidades de 2/8. Es coherente con que la fije el ritmo al que el consumidor
   libera casillas —una cada 2 ms al ritmo agregado de 500 msgs/s, y cuatro
   productores disputándoselas— y no el tamaño del buffer. Lo único que la
   capacidad mueve en ese escenario es la *frecuencia*, y apenas: del 96 % al 86 %
   de las inserciones.
3. **En 5/5 la capacidad sí ataca la frecuencia**: del 41.3 % al **1.2 %** de las
   inserciones bloqueadas, y el costo por inserción cae 30 veces (0.980 → 0.032 ms).
   Ese es, por el lado de la inserción, el mismo hecho que el +18 % de throughput de
   la sección 4.

Del lado de la extracción la figura es simétrica: 6.4–7.0 ms de espera por
extracción y 98 % de extracciones bloqueadas en 8/2, contra 0.03–0.14 ms y menos del
4 % en 2/8. Ese número del consumidor hay que leerlo con la advertencia de la
sección 7: su espera incluye la espera final hasta el cierre.

**La exclusión mutua no es el costo.** Adquirir el mutex cuesta **menos de 1 µs por
operación** en casi todas las celdas (0.24–0.63 µs los productores; 0.35–1.7 µs los
consumidores, con una celda ruidosa de 3.80 ± 5.25 en 2/8 con capacidad 1), o sea
entre tres y cuatro órdenes de magnitud por debajo de las pausas y de las esperas
por la contraparte. En los escenarios con cola —2/8 y 5/5— todo lo que se mide como
"latencia" es tiempo esperando al otro lado y no tiempo gastado en el protocolo. La
excepción es 8/2: ahí no hay cola, y la latencia media resulta ser el costo de
despertar al consumidor dormido (sección 5), que es sincronización pero del
scheduler y no del mutex.

## 4. Lectura: ¿aumentar la capacidad mejora, acumula o desplaza?

**Las tres cosas, y cuál de ellas ocurre lo decide el balance entre los lados.**

### El mecanismo: solo se puede esperar en un borde

Un productor se bloquea únicamente si el buffer está **lleno**; un consumidor,
únicamente si está **vacío**. En cualquier nivel intermedio nadie puede bloquearse.
Así que "¿por qué baja la espera al subir la capacidad?" es en realidad "¿cuánto
tiempo pasa el nivel pegado a un borde?". El modelo de la sección 5 lo mide:

| 5/5 ms | Vacío (n=0) | Lleno (n=C) | **Zona media** | Ocupación media |
|---|---|---|---|---|
| cap 1 | 53.6 % | 46.4 % | **0.0 %** | 0.46 |
| cap 8 | 31.5 % | 21.0 % | 47.5 % | 3.57 |
| cap 64 | 17.1 % | 1.0 % | **81.8 %** | 17.54 |

Con capacidad 1 la zona media es 0 % y no por azar: los únicos niveles posibles son 0
y 1, y 1 *es* el estado lleno. Un buffer de capacidad 1 no amortigua nada — es un
encuentro sincrónico donde cada entrega le cuesta una espera a alguien. Con capacidad
64 el nivel pasa el 81.8 % del tiempo donde ninguno de los dos lados puede bloquearse.

Lo que decide si el nivel llega a visitar esa zona es la **deriva**. Con medias
iguales no hay deriva: el nivel hace un paseo aleatorio y explora. Con medias
distintas hay deriva y el nivel se clava contra un borde:

| Celda | Vacío | Lleno | Zona media |
|---|---|---|---|
| 2/8 cap 64 | 0.2 % | **88.4 %** | 11.4 % |
| 8/2 cap 8 | **99.6 %** | 0.0 % | 0.4 % |
| 8/2 cap 64 | **99.6 %** | 0.0 % | 0.4 % |

Ahí la zona media existe pero el buffer **nunca la visita**: la cruza una vez al
arrancar y no vuelve. En 8/2 el reparto es idéntico con capacidad 8 y con 64, y por
eso esas barras de la figura 3 son perfectamente planas.

> La capacidad crea una zona donde nadie puede bloquearse. Que el buffer se siente en
> ella depende de que las dos tasas estén balanceadas.

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

El mecanismo está en las tablas de esperas (3.3 y 3.4): con capacidad 1, **los dos lados pierden
tiempo a la vez** —el productor se bloquea 83 veces y el consumidor 87, el 16 % y
el 21 % de su vida— sin que ninguno esté saturado. Esa es la firma del
acoplamiento: como las pausas fluctúan tanto como su media, cada fluctuación de un
lado frena al otro, y ese tiempo no se recupera. Con capacidad 64 el productor
deja de bloquearse (0.6 % de su vida, 2 bloqueos por corrida) y queda solo el 9 %
del consumidor, que es la fluctuación que ni 64 casillas alcanzan a absorber.

**El buffer no acelera a nadie: absorbe varianza.** Y solo hay varianza que
absorber cuando ninguno de los dos lados es el cuello de botella. Por eso la misma
capacidad que no aporta nada en 2/8 recupera la mitad del déficit en 5/5.

**Queda un residuo, y es asimétrico.** Con capacidad 64 el productor baja a 0.6 %
pero el consumidor se queda en 9.0 %. La tabla de niveles explica por qué: el tiempo
*lleno* se derrumba (46.4 % → 1.0 %) mientras el tiempo *vacío* baja mucho menos
(53.6 % → 17.1 %). La ocupación media con 64 casillas es 17.5, no 32 — el buffer
arranca vacío y la corrida es finita (800 mensajes, excursión típica √800 ≈ 28), así
que el nivel nunca se equilibra en el medio del rango y queda más cerca del piso.
Dicho de otro modo: **con capacidad grande el techo deja de existir para el
productor, pero el piso nunca deja de existir para el consumidor**, porque un buffer
no puede tener menos de cero mensajes.

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

### La identidad que liga las figuras 1 y 3

Lo que le falta al throughput para llegar al techo es el tiempo que el cuello de
botella pasó sin trabajar:

| Celda | % del techo | Ocio del lado lento | Suma |
|---|---|---|---|
| 2/8 cap 1 | 97.6 % | 1.7 % (consumidor) | **99.3** |
| 2/8 cap 8 | 98.5 % | 0.5 % | **98.9** |
| 5/5 cap 1 | 76.4 % | 20.9 % | **97.4** |
| 5/5 cap 8 | 86.9 % | 13.3 % | **100.3** |
| 5/5 cap 64 | 90.2 % | 9.0 % | **99.2** |

Suma ≈ 100 en todas: las dos figuras son dos vistas del mismo número. El throughput
que no se logró no se gastó en el protocolo, se fue en el lado lento esperando a su
contraparte. El 10 % que le falta al 5/5 con capacidad 64 para llegar a su techo es
el 9 % de su vida que el consumidor sigue pasando dormido en `not_empty` — el mismo
residuo del piso que no desaparece.

## 5. Modelo de referencia: qué parte es el régimen y qué parte la implementación

Una medición sola no distingue "esto lo causa el régimen productor-consumidor" de
"esto lo causa nuestra implementación". `scripts/modelo.py` simula el mismo
experimento por eventos discretos —4+4 procesos, pausas exponenciales, buffer de
capacidad C— **sin mutex, sin CSV y sin llamadas al sistema**: despertar a un proceso
dormido es instantáneo. Lo que el modelo reproduce es del régimen; lo que le falta
para llegar a lo medido es costo real de la implementación o del sistema operativo.

| Celda | thr mod | thr med | lat mod | lat med | prod mod/med | cons mod/med |
|---|---|---|---|---|---|---|
| 2/8 cap 1 | 500.9 | 488.1 | 1.972 | 1.954 | 74.4 / 73.3 % | 0.9 / 1.7 % |
| 2/8 cap 8 | 500.4 | 492.4 | 15.838 | 15.992 | 74.4 / 73.4 % | 0.7 / 0.5 % |
| 2/8 cap 64 | 501.1 | 473.2 | 120.865 | 128.047 | 72.2 / 71.2 % | 0.7 / 0.3 % |
| 5/5 cap 1 | 644.2 | 611.4 | 0.721 | 0.774 | 15.2 / 15.8 % | 19.5 / 20.9 % |
| 5/5 cap 8 | 697.5 | 695.4 | 5.119 | 4.773 | 6.8 / 6.1 % | 13.2 / 13.3 % |
| 5/5 cap 64 | 737.1 | 721.5 | 23.768 | 24.617 | 0.3 / 0.6 % | 8.0 / 9.0 % |
| 8/2 cap 1 | 462.0 | 459.5 | **0.009** | **0.079** | 0.0 / 0.1 % | 76.9 / 75.3 % |
| 8/2 cap 8 | 465.8 | 477.0 | **0.012** | **0.078** | 0.0 / 0.0 % | 76.7 / 74.8 % |
| 8/2 cap 64 | 465.8 | 445.4 | **0.012** | **0.062** | 0.0 / 0.0 % | 76.7 / 77.1 % |

Throughput y esperas se reproducen en las nueve celdas; la latencia, en seis.

### La columna 8/2 de la latencia no mide el buffer

El modelo que acierta las otras seis celdas explica **el 15 %** de esa columna
(0.009-0.012 ms contra 0.062-0.079 medidos), y su percentil 95 es exactamente 0.000:
en el modelo el 95 % de los mensajes tiene latencia nula, porque al insertarse ya hay
un consumidor esperando. Los datos lo confirman del otro lado — el consumidor se
bloquea en **197 de 200** extracciones.

Lo que mide esa columna es el **costo de despertar al consumidor dormido**:
`cond_signal` → el kernel lo marca ejecutable → el scheduler lo despacha →
`pthread_cond_wait` reacquiere el mutex → retorna. Unos 50-70 µs en este entorno. No
es el mutex: `lock_per_op_cons` vale 0.89-1.74 µs, dos órdenes de magnitud menos. Lo
caro es el cambio de contexto, no la exclusión mutua.

Ese costo no aparece en `lock_acquire`: según `sync_get` (`src/sync.c`), esa métrica
mide solo la adquisición inicial, mientras que todo el tiempo dentro de
`pthread_cond_wait` —incluida la readquisición del mutex al despertar— se acumula en
`wait_space`/`wait_data`. Es decir que los ~60 µs por despertar están dentro de las
barras de la figura 3, no en la métrica de bloqueo: en 8/2 son `197 × 60 µs ≈ 12 ms`
sobre una espera de ~1.33 s, el 0.9 %. Mismo costo físico, invisible en una figura y
dominante en la otra, según se mire acumulado o por mensaje.

Eso explica las tres rarezas de la columna a la vez: es diminuta (no hay cola que
atravesar), es plana contra la capacidad (despertar un proceso no depende del tamaño
del buffer) y es la única celda donde el parámetro que varía el experimento no afecta
al resultado. Con una inversión curiosa: el **máximo** sí lo explica el modelo
(1.62-1.87 ms contra 1.47-1.69 medidos, ráfagas raras con cola genuina) pero la
**media** no. En esa celda la media mide el scheduler y el máximo mide el buffer.

### Subir la capacidad satura, no converge

| Capacidad | 2/8 | 5/5 | 8/2 |
|---|---|---|---|
| 1 | 500.9 | 644.2 | 462.0 |
| 8 | 500.4 | 697.5 | 465.8 |
| 32 | 500.5 | 731.0 | 465.8 |
| 64 | 501.1 | 737.1 | 465.8 |
| 128 | 498.6 | **737.6** | 465.8 |
| 256 | 499.9 | **737.6** | 465.8 |
| 1024 | 503.5 | **737.6** | 465.8 |
| ∞ (10⁷) | 503.5 | **737.6** | 465.8 |

El 5/5 gana +0.48 msgs/s de capacidad 64 a 128 y después **exactamente +0.00** hasta
capacidad infinita; el 8/2 satura en ~16; el 2/8 estaba saturado desde 1. No es una
curva que se acerca sin llegar: es una curva que **deja de moverse**. La razón es que
el buffer absorbe un paseo aleatorio cuya excursión sobre la corrida es ~√N, y en
cuanto la capacidad supera la excursión más grande que el sistema va a producir, las
casillas extra **nunca se tocan**.

Consecuencia para este experimento: **capacidad 64 ya estaba en el plateau**, así que
probar 256 o 1024 no habría aportado nada. Eso responde por adelantado por qué el
barrido se detuvo ahí.

### El déficit que queda decae con los mensajes, no con la capacidad

Con capacidad infinita el 5/5 se estaciona en 92.2 % del techo. Ese residuo es el
estadístico de orden de la cuota por productor (sección 3.1), y solo baja alargando
la corrida:

| N/prod | % del techo | Déficit | `1.03/√N` predice |
|---|---|---|---|
| 50 | 87.8 % | 12.17 % | 14.55 % |
| 200 | 92.2 % | 7.80 % | 7.28 % |
| 800 | 96.3 % | 3.74 % | 3.64 % |
| 3200 | 98.3 % | 1.74 % | 1.82 % |

El ajuste da `déficit ~ N^-0.452`, consistente con la ley `1/√N` (que predice −0.500)
y **no** con un decaimiento exponencial. La diferencia es práctica: bajar el déficit
del 7.8 % actual a 1 % pediría ~10 600 mensajes por productor, 50 veces los 200 del
experimento. Y con N finito nunca llega a cero, porque el techo se calculó con la
*media* de la exponencial y la media muestral solo converge en el límite.

Reencuadrado así, el techo realista del 5/5 con 200 mensajes es `800 / 1.073 ≈ 746
msgs/s`, y los **721.5 medidos con capacidad 64 son el 97 % de lo alcanzable** en una
corrida de ese tamaño, no el 90 % que sugiere compararlos contra 800.

## 6. Efecto de los logs sobre la medición

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

## 7. Qué no se puede concluir de estos datos

- **El p95 del resumen del finisher está cuantizado; el de las tablas de arriba no.**
  El resumen lo aproxima con un histograma de error ≤ 6.25 % (D11), y en 2/8 con
  capacidad 64 las 5 repeticiones caen en la misma cubeta (de 151 a 168 ms): las
  cinco reportan su punto medio, 159.38 ms, y la desviación sale **0.00**, que parece
  repetibilidad perfecta y es solo el ancho de la cubeta. Las tablas de la sección 3.2
  usan el p95 exacto recalculado desde los CSV de eventos, y esa celda da
  **160.52 ± 5.11**. O sea que la media del histograma era buena (−0.7 %) y lo que
  perdía era la desviación entera. Quien lea `latency_p95` de un resumen suelto, o del
  agregado hecho sin los CSV de eventos, tiene que saberlo.
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
  la nota de la fila `wait_data`, D28), pero con el protocolo actual eso **vale ~0.2
  puntos de esos 75**. El modelo mide la ventana entre la última extracción y el
  retiro: 0.23 % de la vida del consumidor, porque `experimento.py` lanza el finisher
  en cuanto terminan los productores y la ventana es solo el `fork`/`exec` más el
  `attach`. Medido contra retardos mayores: 50 ms tarde infla 0.7 puntos, 200 ms
  infla 2.4 y 1 s infla 8.6. Así que ese 75 % es hambre genuina en más del 99 %, y la
  validez del dato depende de una propiedad del script, no de la suerte. La asimetría
  estructural que queda es que solo el consumidor puede esperar el corte, porque solo
  el productor tiene cuota con la cual retirarse por su cuenta.

- **La latencia media de 8/2 no mide el buffer** sino el camino de despertar del
  sistema operativo (~60 µs; sección 5). No debe compararse con las otras dos
  columnas como si fuera la misma magnitud, ni leerse como que "el diseño logra
  0.06 ms de latencia". En esa celda el máximo sí es encolamiento, pero la media no.

- **Las figuras muestran fracciones, no cantidades.** Dos barras de altura parecida
  en la figura 3 pueden tener estructuras opuestas: en 2/8 el consumidor se bloquea
  **1 vez** por corrida y en 8/2, **197**. Esa distinción está en la tabla 3.3 entre
  paréntesis, no en el dibujo.
- **5 repeticiones y un solo entorno.** Las desviaciones de la tabla son de este
  equipo, bajo un hipervisor, con otras cargas del sistema compitiendo. Son
  suficientes para separar efectos grandes (el +18 % de 5/5, el factor 66 de
  latencia) y no para discutir diferencias de pocos por ciento.

## 8. Cómo reproducir

```bash
make                                    # los cinco ejecutables
python3 scripts/experimento.py          # las 45 corridas (~70 s)
python3 scripts/analisis.py --archivar  # agregados + copia de los resúmenes
python3 scripts/graficas.py             # las tres figuras SVG
python3 scripts/efecto_logs.py          # el A/B de los logs (~20 s)
python3 scripts/modelo.py               # el modelo de referencia (~10 s)
```

`modelo.py` no depende del experimento: es una simulación y corre sola, así que las
tablas de la sección 5 se reproducen sin volver a medir nada.

El análisis también se repite **sin volver a correr el experimento**, a partir de
los resúmenes versionados en el repositorio:

```bash
python3 scripts/analisis.py --results results/seleccionados
```

Por ese camino el p95 sale **solo aproximado**: D7 excluye del versionado los CSV de
eventos, que son los que permiten el cálculo exacto, y `analisis.py` lo dice en su
salida en vez de cambiar el significado de la métrica en silencio.

Las semillas se derivan de `--base-seed`, así que repetir el experimento con la
misma base reproduce las mismas secuencias de pausas (D27).
