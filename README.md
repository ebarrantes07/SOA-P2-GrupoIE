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

## Entorno objetivo

- GNU/Linux de 64 bits
- GCC y GNU make
- C17 con APIs POSIX (memoria compartida `shm_open`/`mmap`, primitivas de sincronización
  configuradas para compartirse entre procesos)

## Compilación

```bash
make          # compila los ejecutables: creator, producer, consumer, finisher
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

El resto del código es original de este proyecto.

## Uso

_Se completa a medida que se implementan los ejecutables (interfaz mínima del enunciado,
formatos CSV y protocolo de ejecución y cierre)._

### creator

```bash
./creator --name /soa_p2_g1 --capacity 8
```

Crea e inicializa la instancia y termina con código 0 cuando ya acepta participantes. El
nombre es `/` seguido de 1 a 30 caracteres `[A-Za-z0-9_-]` y la capacidad va de 1 a 1024.
Si ya existe una instancia con ese nombre, la rechaza sin modificarla (código 1).

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

## Versión entregada

_Pendiente: URL, etiqueta `p2-entrega` y hash completo del commit._
