#!/usr/bin/env python3
"""Analisis del experimento (Issue #25, decisiones en D32).

Lee results/manifiesto.csv y el resumen de cada corrida, agrega las 5
repeticiones de cada combinacion (media y desviacion estandar) y escribe
results/seleccionados/agregados.csv, que es lo que cita el informe.

    python3 scripts/analisis.py
    python3 scripts/analisis.py --results results --out results/seleccionados
    python3 scripts/analisis.py --archivar   # copia los resumenes citados

No calcula nada que el finisher ya haya medido: toma sus filas tal cual
(D11, D28) y solo agrega entre repeticiones. Lo unico que deriva son
promedios por participante y las esperas como fraccion de la vida del
proceso, que es la forma en que se pueden comparar corridas de distinta
duracion.

Codigo de salida: 0 si el experimento era valido y el analisis se escribio,
1 si alguna corrida no era usable o faltan datos.
"""
import argparse
import csv
import os
import shutil
import statistics
import sys

# ---- Metricas que se agregan --------------------------------------------

# (clave, unidad, decimales). El orden es el de la tabla que se imprime.
METRICS = [
    ("throughput",       "msgs/s", 1),
    ("latency_mean",     "ms",     2),
    ("latency_p95",      "ms",     2),
    ("latency_max",      "ms",     2),
    ("wait_space",       "ms",     1),
    ("wait_data",        "ms",     1),
    ("wait_space_frac",  "%",      1),
    ("wait_data_frac",   "%",      1),
    ("wait_count_prod",  "count",  1),
    ("wait_count_cons",  "count",  1),
    ("lock_acquire",     "us",     1),
    ("active_interval",  "s",      3),
    ("close_duration",   "ms",     3),
]

NS_PER_MS = 1e6
NS_PER_US = 1e3
NS_PER_S = 1e9


def die(msg):
    print(f"analisis: {msg}", file=sys.stderr)
    sys.exit(1)


# ---- Lectura -------------------------------------------------------------

def read_manifest(results_dir):
    """Las filas del manifiesto, en el orden en que se corrieron."""
    path = os.path.join(results_dir, "manifiesto.csv")
    if not os.path.exists(path):
        die(f"falta {path}; corra primero scripts/experimento.py")
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        die(f"{path} no tiene corridas")
    return rows


def summary_path(results_dir, run_id):
    """Acepta el layout del experimento y el de los resumenes archivados."""
    for candidate in (os.path.join(results_dir, run_id, "resumen.csv"),
                      os.path.join(results_dir, "resumenes", run_id + ".csv")):
        if os.path.exists(candidate):
            return candidate
    die(f"no encuentro el resumen de {run_id} en {results_dir}")


def read_summary(path):
    """El resumen de una corrida: filas de instancia y por participante.

    Devuelve (instance, participants), donde instance es {metrica: valor} y
    participants es {(rol, id): {metrica: valor}}.
    """
    instance = {}
    participants = {}
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            try:
                value = float(row["value"])
            except (TypeError, ValueError):
                die(f"{path}: valor no numerico en la fila {row}")
            if row["role"] == "instance":
                instance[row["metric"]] = value
            else:
                key = (row["role"], int(row["id"]))
                participants.setdefault(key, {})[row["metric"]] = value
    if not instance:
        die(f"{path}: sin filas de instancia")
    return instance, participants


# ---- Validacion ----------------------------------------------------------

def check_run(run, instance):
    """Una corrida que no conserva los mensajes no se analiza: se denuncia.

    El enunciado mide rendimiento de un buffer *correcto*; promediar una
    corrida con perdidas daria numeros que no significan nada.
    """
    if run["status"] != "ok":
        return f"status={run['status']} ({run['detail']})"
    if run["verify_rc"] != "0":
        return f"verify devolvio {run['verify_rc']}"
    if instance.get("invariant_ok") != 1.0:
        return "invariant_ok=0 en el resumen"
    if instance.get("pending") != 0.0:
        return f"quedaron {instance.get('pending')} mensajes pendientes"
    expected = int(run["producers"]) * int(run["messages_per_producer"])
    if instance.get("accepted") != expected or instance.get("extracted") != expected:
        return (f"aceptados={instance.get('accepted')} extraidos="
                f"{instance.get('extracted')}, se esperaban {expected}")
    return None


# ---- Metricas de una corrida --------------------------------------------

def mean_of(participants, role, metric):
    values = [p[metric] for (r, _), p in participants.items() if r == role and metric in p]
    if not values:
        die(f"ninguna fila {metric} de {role}")
    return statistics.fmean(values)


def wait_fraction(participants, role, wait_metric):
    """Espera acumulada del rol como fraccion de su vida, en por ciento.

    Es la forma comparable entre corridas: los tiempos absolutos de dos
    escenarios con medias distintas no se pueden poner uno al lado del otro,
    porque las corridas no duran lo mismo.
    """
    waited = sum(p[wait_metric] for (r, _), p in participants.items()
                 if r == role and wait_metric in p)
    lived = sum(p["lifetime"] for (r, _), p in participants.items()
                if r == role and "lifetime" in p)
    if lived <= 0:
        die(f"vida total de {role} no positiva")
    return 100.0 * waited / lived


def run_metrics(instance, participants):
    """Las metricas de una corrida, ya en unidades legibles."""
    return {
        "throughput":      instance["throughput"],
        "latency_mean":    instance["latency_mean"] / NS_PER_MS,
        "latency_p95":     instance["latency_p95"] / NS_PER_MS,
        "latency_max":     instance["latency_max"] / NS_PER_MS,
        "wait_space":      mean_of(participants, "producer", "wait_space") / NS_PER_MS,
        "wait_data":       mean_of(participants, "consumer", "wait_data") / NS_PER_MS,
        "wait_space_frac": wait_fraction(participants, "producer", "wait_space"),
        "wait_data_frac":  wait_fraction(participants, "consumer", "wait_data"),
        "wait_count_prod": mean_of(participants, "producer", "wait_count"),
        "wait_count_cons": mean_of(participants, "consumer", "wait_count"),
        "lock_acquire":    (mean_of(participants, "producer", "lock_acquire")
                            + mean_of(participants, "consumer", "lock_acquire")) / 2 / NS_PER_US,
        "active_interval": instance["active_interval"] / NS_PER_S,
        "close_duration":  instance["close_duration"] / NS_PER_MS,
    }


# ---- Agregacion ----------------------------------------------------------

def load(results_dir):
    """{(capacidad, media_prod, media_cons): [metricas de cada repeticion]}"""
    groups = {}
    for run in read_manifest(results_dir):
        path = summary_path(results_dir, run["run_id"])
        instance, participants = read_summary(path)
        problem = check_run(run, instance)
        if problem is not None:
            die(f"corrida {run['run_id']} no usable: {problem}")
        key = (int(run["capacity"]), int(run["mean_prod_ms"]), int(run["mean_cons_ms"]))
        groups.setdefault(key, []).append(run_metrics(instance, participants))
    return groups


def aggregate(groups):
    """{(clave de grupo): {metrica: (media, desviacion, n)}}

    La desviacion es la muestral (n-1): las 5 repeticiones son una muestra
    de las corridas posibles, no la poblacion entera.
    """
    out = {}
    for key, reps in sorted(groups.items()):
        stats = {}
        for name, _unit, _dec in METRICS:
            values = [r[name] for r in reps]
            sd = statistics.stdev(values) if len(values) > 1 else 0.0
            stats[name] = (statistics.fmean(values), sd, len(values))
        out[key] = stats
    return out


# ---- Salida --------------------------------------------------------------

def scenario_label(mean_prod, mean_cons):
    return f"{mean_prod}/{mean_cons}"


def print_tables(agg):
    """Una tabla por metrica: capacidades en filas, escenarios en columnas."""
    capacities = sorted({k[0] for k in agg})
    scenarios = sorted({(k[1], k[2]) for k in agg})
    n = max(agg[k][METRICS[0][0]][2] for k in agg)
    print(f"Media +/- desviacion estandar entre {n} repeticiones\n")
    for name, unit, dec in METRICS:
        print(f"{name} [{unit}]")
        head = "  cap |" + "".join(f"{'  prod/cons ' + scenario_label(*s):>22}" for s in scenarios)
        print(head)
        for cap in capacities:
            cells = []
            for s in scenarios:
                mean, sd, _ = agg[(cap, s[0], s[1])][name]
                cells.append(f"{mean:>12.{dec}f} +/-{sd:>7.{dec}f}")
            print(f"  {cap:>3} |" + "".join(f"{c:>22}" for c in cells))
        print()


def write_csv(agg, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, "agregados.csv")
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["capacity", "mean_prod_ms", "mean_cons_ms", "metric", "unit",
                    "mean", "sd", "n"])
        for (cap, mp, mc), stats in sorted(agg.items()):
            for name, unit, _dec in METRICS:
                mean, sd, n = stats[name]
                w.writerow([cap, mp, mc, name, unit, f"{mean:.6f}", f"{sd:.6f}", n])
    return path


def archive(results_dir, out_dir):
    """Copia el manifiesto y los resumenes citados a la carpeta versionada.

    Solo los resumenes: los CSV de eventos son los masivos que D7 excluye.
    Con esto el analisis se puede repetir desde el repositorio, sin volver a
    correr el experimento, apuntando --results a la carpeta de salida.
    """
    dest = os.path.join(out_dir, "resumenes")
    os.makedirs(dest, exist_ok=True)
    shutil.copy2(os.path.join(results_dir, "manifiesto.csv"),
                 os.path.join(out_dir, "manifiesto.csv"))
    count = 0
    for run in read_manifest(results_dir):
        shutil.copy2(summary_path(results_dir, run["run_id"]),
                     os.path.join(dest, run["run_id"] + ".csv"))
        count += 1
    return count, dest


def main():
    ap = argparse.ArgumentParser(description="Analisis del experimento (#25)")
    ap.add_argument("--results", default="results",
                    help="carpeta con manifiesto.csv (por omision: results)")
    ap.add_argument("--out", default=os.path.join("results", "seleccionados"),
                    help="carpeta versionada de salida")
    ap.add_argument("--archivar", action="store_true",
                    help="copia manifiesto y resumenes a la carpeta de salida")
    args = ap.parse_args()

    agg = aggregate(load(args.results))
    print_tables(agg)
    path = write_csv(agg, args.out)
    print(f"agregados: {path}")
    if args.archivar:
        count, dest = archive(args.results, args.out)
        print(f"archivados: {count} resumenes en {dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
