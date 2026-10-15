# Bitácora de decisiones de diseño

Registro vivo de las decisiones del proyecto. Cada entrada se numera (D1, D2, …) e incluye
fecha, issue de origen, contexto, decisión y alternativas descartadas. Las entradas no se
reescriben: si una decisión cambia, se agrega una nueva entrada que la reemplaza y se indica
en la anterior.

---

## D1. Estructura del repositorio y ubicación de los ejecutables

- **Fecha:** 2026-10-15
- **Issue:** #1
- **Contexto:** el enunciado sugiere `src/`, `include/`, `tests/`, `scripts/`, `results/` y
  `docs/`, y muestra los ejecutables invocados como `./creator`, `./producer`, etc.
- **Decisión:** se usan esas carpetas. Cada ejecutable tiene su `main` en `src/<nombre>.c`;
  el resto de `src/*.c` son módulos comunes que se enlazan en los cuatro ejecutables. Los
  ejecutables se generan en la raíz y los objetos en `build/`.
- **Alternativas descartadas:** generar los ejecutables en `bin/`, porque obliga a cambiar
  los comandos de ejemplo del enunciado.

## D2. Flags de compilación y macros de características

- **Fecha:** 2026-10-15
- **Issue:** #1
- **Contexto:** el enunciado exige `-std=c17 -Wall -Wextra -Wpedantic -Werror -pthread` y
  pide añadir las macros de características y bibliotecas necesarias. Con `-std=c17` estricto,
  glibc oculta las declaraciones POSIX si no se define una macro de características.
- **Decisión:** `_POSIX_C_SOURCE=200809L` en `CPPFLAGS`; se enlaza con `-lm` y, en Linux,
  con `-lrt`.
- **Alternativas descartadas:** `-std=gnu17` o `_GNU_SOURCE`, porque relajan el estándar
  exigido y pueden ocultar dependencias no portables.

## D3. Makefile incremental

- **Fecha:** 2026-10-15
- **Issue:** #1
- **Contexto:** los módulos se implementan en issues distintos a lo largo del proyecto.
- **Decisión:** el Makefile descubre los módulos con `wildcard` y solo construye los
  ejecutables cuyo `main` ya existe, así `make` y `make test` funcionan en cada etapa sin
  editar listas de archivos. Genera dependencias de headers con `-MMD -MP`. `test` es un
  placeholder hasta los issues de pruebas (#19–#22).
- **Alternativas descartadas:** listar archivos a mano, porque genera conflictos de merge
  entre PRs paralelos.

## D4. Resultados versionados

- **Fecha:** 2026-10-15
- **Issue:** #1
- **Contexto:** el enunciado pide excluir resultados masivos pero conservar los necesarios
  para reproducir el informe.
- **Decisión:** `.gitignore` excluye `results/*` salvo `results/seleccionados/`, donde se
  guardan los CSV, semillas y gráficas citados en el informe.
