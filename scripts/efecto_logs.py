#!/usr/bin/env python3
"""Efecto de los logs sobre la medicion (Issue #25, criterio 4; ver D32).

El enunciado pide documentar cuanto distorsiona la propia instrumentacion
lo que se esta midiendo. Se mide con un A/B: la misma corrida con los CSV
en un archivo de verdad y con los CSV a /dev/null, en DOS regimenes:

    pausas 5/5 ms   el del experimento (#24): la pausa simulada domina
    sin pausas      --mean-ms 0: el costo por operacion es todo lo que hay

El segundo regimen es el que puede mostrar el costo; el primero es el que
dice si ese costo contamina los numeros del informe.

    python3 scripts/efecto_logs.py              # 5 repeticiones por celda
    python3 scripts/efecto_logs.py --reps 3

Reutiliza la orquestacion de scripts/experimento.py (lanzar, esperar,
limpiar) en vez de repetirla: lo unico que cambia es el destino de --log.

Lo que mide y lo que NO: con /dev/null el logger sigue formateando cada
fila (snprintf) y sigue capturando los campos bajo el mutex; lo que
desaparece es la escritura al archivo. El A/B aisla ese costo, no el del
formateo.
"""
import argparse
import os
import shutil
import statistics
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import experimento as exp                                    # noqa: E402

# (etiqueta, media del productor, media del consumidor, mensajes por
# productor). El regimen con pausas copia el del experimento (200 mensajes,
# como el #24). El de sin pausas necesita MAS mensajes: a ~95 000 msgs/s,
# 800 mensajes duran 8 ms y el arranque de los nueve procesos domina el
# ruido (medido: desviacion del 14 %). Con 5000 por productor la corrida
# dura ~200 ms y el costo por operacion es lo que fija la tasa.
REGIMES = [("pausas 5/5", 5, 5, 200), ("sin pausas", 0, 0, 5000)]
TARGETS = [("archivo", False), ("/dev/null", True)]
CAPACITY = 8
BASE_SEED = 20261028


def one_run(root, out_dir, name, mean_prod, mean_cons, messages, to_devnull,
             run_index):
    """Una corrida; devuelve (throughput, latencia_media_ms, bytes de CSV)."""
    # Se borra y se recrea: si quedaran CSV de una invocacion anterior, el
    # conteo de bytes escritos los sumaria y una celda /dev/null informaria
    # haber escrito archivos (pasó mientras se escribia esta medicion).
    run_dir = os.path.join(out_dir, f"run{run_index}")
    if os.path.isdir(run_dir):
        shutil.rmtree(run_dir)
    os.makedirs(run_dir)

    def log_path(who):
        return os.devnull if to_devnull else os.path.join(run_dir, f"{who}.csv")

    pseeds = exp.seeds_for(BASE_SEED, run_index, 0, exp.PRODUCERS)
    cseeds = exp.seeds_for(BASE_SEED, run_index, 500, exp.CONSUMERS)
    launched, handles = [], []
    try:
        rc = subprocess.run([os.path.join(root, "creator"), "--name", name,
                             "--capacity", str(CAPACITY)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                            timeout=30)
        if rc.returncode != 0:
            raise exp.RunFailure("creator: " + rc.stderr.decode().strip())

        cons = []
        for i in range(exp.CONSUMERS):
            p, fh = exp.spawn(root, "consumer", [
                "--name", name, "--id", str(i + 1), "--mean-ms", str(mean_cons),
                "--seed", str(cseeds[i]), "--log", log_path(f"c{i + 1}"),
            ], os.path.join(run_dir, f"c{i + 1}.out"))
            cons.append(p); launched.append(p); handles.append(fh)

        prods = []
        for i in range(exp.PRODUCERS):
            p, fh = exp.spawn(root, "producer", [
                "--name", name, "--id", str(i + 1), "--mean-ms", str(mean_prod),
                "--seed", str(pseeds[i]), "--messages", str(messages),
                "--log", log_path(f"p{i + 1}"),
            ], os.path.join(run_dir, f"p{i + 1}.out"))
            prods.append(p); launched.append(p); handles.append(fh)

        rcs = exp.wait_all(prods, exp.timeout_for(mean_prod, messages,
                                                  exp.PRODUCERS), "los productores")
        if any(r != 0 for r in rcs):
            raise exp.RunFailure(f"productores con codigo {rcs}")

        summary = os.path.join(run_dir, "resumen.csv")
        fin = subprocess.run([os.path.join(root, "finisher"), "--name", name,
                              "--summary", summary, "--log", log_path("fin")],
                             stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                             timeout=120)
        if fin.returncode != 0:
            raise exp.RunFailure("finisher: " + fin.stderr.decode().strip())

        rcs = exp.wait_all(cons, exp.timeout_for(mean_cons,
                                                 messages * exp.PRODUCERS,
                                                 exp.CONSUMERS), "los consumidores")
        if any(r != 0 for r in rcs):
            raise exp.RunFailure(f"consumidores con codigo {rcs}")

        values = {}
        with open(summary) as f:
            for line in f.read().splitlines()[1:]:
                parts = line.split(",")
                if parts[1] == "instance":
                    values[parts[0]] = float(parts[3])
        written = sum(os.path.getsize(os.path.join(run_dir, f))
                      for f in os.listdir(run_dir) if f.endswith(".csv")
                      and f != "resumen.csv")
        return values["throughput"], values["latency_mean"] / 1e6, written
    finally:
        exp.kill_survivors(launched)
        for fh in handles:
            fh.close()
        exp.cleanup_segment(name)


def main():
    ap = argparse.ArgumentParser(description="Efecto de los logs (#25)")
    ap.add_argument("--reps", type=int, default=7)
    ap.add_argument("--out", default=os.path.join("results", "efecto_logs"))
    ap.add_argument("--csv", default=os.path.join("results", "seleccionados",
                                                  "efecto_logs.csv"))
    args = ap.parse_args()
    root = os.getcwd()
    for exe in exp.BINARIES:
        if not os.access(os.path.join(root, exe), os.X_OK):
            print(f"efecto_logs: falta ./{exe}; corra make primero", file=sys.stderr)
            return 1
    os.makedirs(args.out, exist_ok=True)

    rows = []
    index = 0
    for label, mp, mc, messages in REGIMES:
        for target, to_devnull in TARGETS:
            tputs, lats, bytes_written = [], [], []
            for rep in range(args.reps):
                index += 1
                name = f"/log{os.getpid() % 100000}_{index}"
                t, lat, written = one_run(root, args.out, name, mp, mc, messages,
                                          to_devnull, index)
                tputs.append(t); lats.append(lat); bytes_written.append(written)
            rows.append({
                "regimen": label, "destino": target, "n": args.reps,
                "mensajes": messages * exp.PRODUCERS,
                "throughput_mean": statistics.fmean(tputs),
                "throughput_sd": statistics.stdev(tputs) if args.reps > 1 else 0.0,
                "latency_mean_ms": statistics.fmean(lats),
                "bytes_csv": statistics.fmean(bytes_written),
            })
            print(f"{label:>12} | {target:>10} | throughput "
                  f"{rows[-1]['throughput_mean']:8.1f} +/-{rows[-1]['throughput_sd']:6.1f} "
                  f"| latencia {rows[-1]['latency_mean_ms']:7.3f} ms "
                  f"| CSV {rows[-1]['bytes_csv'] / 1024:7.1f} KiB", flush=True)

    print()
    for label, _mp, _mc, _msgs in REGIMES:
        a = next(r for r in rows if r["regimen"] == label and r["destino"] == "archivo")
        b = next(r for r in rows if r["regimen"] == label and r["destino"] == "/dev/null")
        delta = 100.0 * (b["throughput_mean"] - a["throughput_mean"]) / a["throughput_mean"]
        # Error estandar de la diferencia de dos medias independientes.
        se = ((a["throughput_sd"] ** 2 + b["throughput_sd"] ** 2) / a["n"]) ** 0.5
        sigmas = abs(b["throughput_mean"] - a["throughput_mean"]) / se if se > 0 else 0.0
        print(f"{label}: sin escribir los CSV el throughput cambia {delta:+.1f} % "
              f"({sigmas:.1f} errores estandar)")

    os.makedirs(os.path.dirname(args.csv), exist_ok=True)
    with open(args.csv, "w", newline="\n") as f:
        f.write("regimen,destino,n,mensajes,throughput_mean,throughput_sd,"
                "latency_mean_ms,bytes_csv\n")
        for r in rows:
            f.write(f"{r['regimen']},{r['destino']},{r['n']},{r['mensajes']},"
                    f"{r['throughput_mean']:.3f},{r['throughput_sd']:.3f},"
                    f"{r['latency_mean_ms']:.4f},{r['bytes_csv']:.0f}\n")
    print(f"\nmedicion: {args.csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
