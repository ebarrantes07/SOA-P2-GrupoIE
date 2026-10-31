# Informe (fuente LaTeX)

Fuente del informe del proyecto (`docs/informe.pdf`, de 8 a 12 páginas sin
anexos según el enunciado). Formato `IEEEtran`, igual que el Proyecto 1: una
sección por archivo `.tex`, referencias en `referencias.bib`.

| Archivo | Sección | Responsable |
|---|---|---|
| `introduccion.tex` | Introducción | ambos |
| `arquitectura.tex` | Arquitectura y representación de memoria | Isaac (#26) |
| `protocolo.tex` | Protocolo de sincronización y cierre (pseudocódigo) | Isaac (#26) |
| `pruebas.tex` | Pruebas y verificación | Isaac (#26) |
| `resultados.tex` | Resultados del experimento | Emmanuel (#25) |
| `lecturas.tex` | Relación con Dijkstra y Birrell | Emmanuel (#27) |
| `decisiones.tex` | Discusión de decisiones de diseño | Emmanuel (#27) |
| `conclusiones.tex` | Conclusiones | ambos |
| `declaracion_transparencia.tex`, `entrega.tex` | Uso de IA, material reutilizado, URL/etiqueta/hash | ambos |

La gestión del proyecto y la trazabilidad (responsabilidades, Issues, Pull
Requests y revisiones) no van en el informe: están en la sección «Gestión y
trazabilidad» del README del repositorio.

Lo que falta escribir aparece en el PDF como **[PENDIENTE — …]** y en el log
de compilación como `PENDIENTE:`. El PDF entregado no debe tener ninguno.

## Compilar

```bash
cd docs/informe
latexmk -pdf main.tex        # o: pdflatex, bibtex, pdflatex, pdflatex
grep -a -c PENDIENTE main.log   # lo que falta (-a: el log no es UTF-8 puro)
cp main.pdf ../informe.pdf
```

## Figuras

Las figuras del experimento se dibujan con `pgfplots` a partir de los `.dat`
de `graficas/`, que salen de `results/seleccionados/agregados.csv` (D33):

```bash
python3 docs/informe/graficas/generar_datos.py   # desde la raíz del repo
```

Los `.dat` se versionan para que el informe compile sin correr nada; hay que
regenerarlos si se vuelve a correr el análisis.
