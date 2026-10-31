#!/usr/bin/env python3
"""Genera los .dat que dibujan las figuras del informe con pgfplots.

Lee results/seleccionados/agregados.csv (lo produce scripts/analisis.py,
D33) y escribe una tabla por métrica: una fila por capacidad y, para cada
escenario (media de pausa productor/consumidor), la media y la desviación
estándar entre las 5 repeticiones.

    python3 docs/informe/graficas/generar_datos.py

Solo biblioteca estándar. Los .dat se versionan para que el informe
compile sin correr nada; regenerarlos tras un nuevo análisis.
"""
import csv
import os
import sys

AQUI = os.path.dirname(os.path.abspath(__file__))
AGREGADOS = os.path.join(AQUI, "..", "..", "..", "results", "seleccionados", "agregados.csv")

CAPACIDADES = (1, 8, 64)
ESCENARIOS = (("2", "8", "e28"), ("5", "5", "e55"), ("8", "2", "e82"))

# archivo .dat -> métrica de agregados.csv
METRICAS = {
    "throughput.dat": "throughput",
    "latencia_media.dat": "latency_mean",
    "latencia_p95.dat": "latency_p95_exact",
    "espera_productor.dat": "wait_space_frac",
    "espera_consumidor.dat": "wait_data_frac",
}


def main():
    valores = {}
    with open(AGREGADOS, newline="") as f:
        for r in csv.DictReader(f):
            clave = (int(r["capacity"]), r["mean_prod_ms"], r["mean_cons_ms"], r["metric"])
            valores[clave] = (float(r["mean"]), float(r["sd"]))

    for archivo, metrica in METRICAS.items():
        cabecera = ["cap"]
        for _, _, nombre in ESCENARIOS:
            cabecera += [nombre, nombre + "sd"]
        filas = [" ".join(cabecera)]
        for cap in CAPACIDADES:
            fila = [str(cap)]
            for mp, mc, _ in ESCENARIOS:
                clave = (cap, mp, mc, metrica)
                if clave not in valores:
                    sys.exit(f"falta {metrica} para capacidad {cap}, escenario {mp}/{mc}")
                media, sd = valores[clave]
                fila += [f"{media:.6f}", f"{sd:.6f}"]
            filas.append(" ".join(fila))
        with open(os.path.join(AQUI, archivo), "w") as f:
            f.write("\n".join(filas) + "\n")
        print(f"{archivo}: {metrica}")


if __name__ == "__main__":
    main()
