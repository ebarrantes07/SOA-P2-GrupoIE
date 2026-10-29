#!/usr/bin/env python3
"""Script del experimento (Issue #24, decisiones en D27).

Automatiza las 45 corridas que pide el enunciado: 3 capacidades x 3
escenarios de medias x 5 repeticiones, con 4 productores y 4 consumidores
de 200 mensajes cada productor.

    make                                   # los cinco ejecutables primero
    python3 scripts/experimento.py         # las 45 corridas
    python3 scripts/experimento.py --quick # 3 corridas, para probar el script

Cada corrida deja sus CSV en results/<id>/ y una fila en
results/manifiesto.csv con sus parametros, sus semillas, los codigos de
salida y el resultado del verificador. El #25 lee ese manifiesto: no
depende de adivinar nombres de archivo.

Codigo de salida: 0 solo si las 45 corridas quedaron validas.
"""
import argparse
import csv
import os
import shutil
import signal
import subprocess
import sys
import time

# ---- Matriz del experimento (enunciado) ---------------------------------

CAPACITIES = [1, 8, 64]
# (media del productor, media del consumidor) en ms
SCENARIOS = [(2, 8), (8, 2), (5, 5)]
REPETITIONS = 5
PRODUCERS = 4
CONSUMERS = 4
MESSAGES_PER_PRODUCER = 200

BINARIES = ["creator", "producer", "consumer", "finisher", "verify"]

# Semilla base por omision. Cada proceso recibe una semilla derivada de
# forma determinista, asi que repetir el experimento con la misma base
# reproduce las mismas secuencias de pausas (criterio del issue).
DEFAULT_BASE_SEED = 20261027

MANIFEST_FIELDS = [
    "run_id", "capacity", "mean_prod_ms", "mean_cons_ms", "rep",
    "producers", "consumers", "messages_per_producer",
    "base_seed", "producer_seeds", "consumer_seeds",
    "status", "verify_rc", "wall_s", "accepted", "extracted", "detail",
]


class RunFailure(Exception):
    """Una corrida que no deja datos usables."""


def log(msg):
    print(msg, flush=True)


# ---- Semillas ------------------------------------------------------------

def seeds_for(base_seed, run_index, role_offset, count):
    """Semillas deterministas, distintas entre roles y corridas, nunca 0.

    xorshift32 se atasca con estado 0 y la CLI lo rechaza (ver rng.h), asi
    que el valor se mantiene en 1..2^32-1.
    """
    out = []
    for i in range(count):
        raw = base_seed + run_index * 1000 + role_offset + i
        out.append(raw % 0xFFFFFFFF + 1)
    return out


# ---- Una corrida ---------------------------------------------------------

def timeout_for(mean_ms, messages, processes):
    """Plazo generoso: lo que tardaria un solo proceso haciendo todo el
    trabajo en serie, por 4, con un piso de 30 s."""
    serial_s = mean_ms * messages * processes / 1000.0
    return max(30.0, serial_s * 4.0)


def wait_all(procs, timeout_s, what):
    """Espera a todos; devuelve sus codigos. Lanza RunFailure si alguno no
    termina a tiempo, despues de matarlos a todos."""
    deadline = time.monotonic() + timeout_s
    for p in procs:
        left = deadline - time.monotonic()
        if left <= 0:
            left = 0.01
        try:
            p.wait(timeout=left)
        except subprocess.TimeoutExpired:
            for q in procs:
                if q.poll() is None:
                    q.kill()
            for q in procs:
                try:
                    q.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
            raise RunFailure(f"plazo vencido esperando {what} ({timeout_s:.0f} s)")
    return [p.returncode for p in procs]


def spawn(root, exe, args, logfile):
    """Lanza un ejecutable del proyecto con su salida a un archivo."""
    cmd = [os.path.join(root, exe)] + args
    out = open(logfile, "w")
    return subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT), out


def kill_survivors(procs):
    """Mata lo que siga vivo y devuelve cuantos eran.

    Hace falta en toda corrida que no termino bien: los consumidores se
    quedan esperando en not_empty mientras la instancia siga en OPEN, y el
    corte nunca llega porque el finisher no se lanza (con un participante
    muerto se colgaria para siempre, ver #50). Sin esto, cada corrida
    invalida deja 4 consumidores vivos: medido, 45 corridas fallidas
    dejarian 180 procesos.
    """
    alive = [p for p in procs if p.poll() is None]
    for p in alive:
        p.kill()
    for p in alive:
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
    return len(alive)


def run_once(root, out_dir, run_id, capacity, mean_prod, mean_cons, rep,
             run_index, base_seed):
    """Una corrida completa. Devuelve la fila del manifiesto."""
    run_dir = os.path.join(out_dir, run_id)
    if os.path.isdir(run_dir):
        shutil.rmtree(run_dir)
    os.makedirs(run_dir)

    # El nombre tiene que caber en 30 caracteres despues de '/' (D13).
    name = f"/exp{os.getpid() % 100000}_{run_index}"
    pseeds = seeds_for(base_seed, run_index, 0, PRODUCERS)
    cseeds = seeds_for(base_seed, run_index, 500, CONSUMERS)

    row = {
        "run_id": run_id, "capacity": capacity,
        "mean_prod_ms": mean_prod, "mean_cons_ms": mean_cons, "rep": rep,
        "producers": PRODUCERS, "consumers": CONSUMERS,
        "messages_per_producer": MESSAGES_PER_PRODUCER,
        "base_seed": base_seed,
        "producer_seeds": " ".join(str(s) for s in pseeds),
        "consumer_seeds": " ".join(str(s) for s in cseeds),
        "status": "", "verify_rc": "", "wall_s": "",
        "accepted": "", "extracted": "", "detail": "",
    }

    started = time.monotonic()
    handles = []
    launched = []          # todo lo que se lanzo, para el teardown
    try:
        rc = subprocess.run(
            [os.path.join(root, "creator"), "--name", name,
             "--capacity", str(capacity)],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=30)
        if rc.returncode != 0:
            raise RunFailure("creator: " + rc.stderr.decode().strip())

        # Consumidores primero: al cortar hace falta al menos uno activo o
        # closing_begin rechaza el cierre (precondicion de D10).
        cons = []
        for i in range(CONSUMERS):
            p, fh = spawn(root, "consumer", [
                "--name", name, "--id", str(i + 1),
                "--mean-ms", str(mean_cons), "--seed", str(cseeds[i]),
                "--log", os.path.join(run_dir, f"c{i + 1}.csv"),
            ], os.path.join(run_dir, f"c{i + 1}.out"))
            cons.append(p)
            launched.append(p)
            handles.append(fh)

        prods = []
        for i in range(PRODUCERS):
            p, fh = spawn(root, "producer", [
                "--name", name, "--id", str(i + 1),
                "--mean-ms", str(mean_prod), "--seed", str(pseeds[i]),
                "--messages", str(MESSAGES_PER_PRODUCER),
                "--log", os.path.join(run_dir, f"p{i + 1}.csv"),
            ], os.path.join(run_dir, f"p{i + 1}.out"))
            prods.append(p)
            launched.append(p)
            handles.append(fh)

        # Un consumidor que murio al arrancar (ruta mal, id repetido) haria
        # que el cierre se rechace mucho despues; se detecta ya.
        time.sleep(0.2)
        dead = [i + 1 for i, p in enumerate(cons) if p.poll() not in (None, 0)]
        if dead:
            raise RunFailure(f"consumidores {dead} terminaron al arrancar")

        # El cierre se pide cuando terminan TODOS los productores (criterio
        # del issue).
        prod_rcs = wait_all(prods, timeout_for(mean_prod, MESSAGES_PER_PRODUCER,
                                               PRODUCERS), "los productores")
        if any(r != 0 for r in prod_rcs):
            raise RunFailure(f"productores con codigo {prod_rcs}")

        fin = subprocess.run(
            [os.path.join(root, "finisher"), "--name", name,
             "--summary", os.path.join(run_dir, "resumen.csv"),
             "--log", os.path.join(run_dir, "finisher.csv")],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
        with open(os.path.join(run_dir, "finisher.out"), "wb") as f:
            f.write(fin.stdout)
        if fin.returncode != 0:
            raise RunFailure("finisher: " + fin.stdout.decode().strip().replace("\n", "; "))

        cons_rcs = wait_all(cons, timeout_for(mean_cons,
                                              MESSAGES_PER_PRODUCER * PRODUCERS,
                                              CONSUMERS), "los consumidores")
        if any(r != 0 for r in cons_rcs):
            raise RunFailure(f"consumidores con codigo {cons_rcs}")

    except subprocess.TimeoutExpired as exc:
        row["status"] = "invalida"
        row["detail"] = f"plazo vencido: {exc.cmd[0] if exc.cmd else '?'}"
    except RunFailure as exc:
        row["status"] = "invalida"
        row["detail"] = str(exc)
    else:
        row["status"] = "ok"
    finally:
        # Primero los procesos: un consumidor vivo con la instancia en OPEN
        # no se entera de nada y espera para siempre.
        killed = kill_survivors(launched)
        for fh in handles:
            fh.close()
        if row["status"] != "ok":
            if killed:
                row["detail"] += f" [{killed} proceso(s) terminado(s) a la fuerza]"
            # El finisher puede no haber corrido (o no haber podido
            # cerrar): el nombre se libera a mano para no dejar basura en
            # /dev/shm ni chocar con la corrida siguiente.
            cleanup_segment(name)

    row["wall_s"] = f"{time.monotonic() - started:.2f}"

    if row["status"] == "ok":
        csvs = sorted(f for f in os.listdir(run_dir) if f.endswith(".csv")
                      and f != "resumen.csv")
        vr = subprocess.run(
            [os.path.join(root, "verify")] + [os.path.join(run_dir, f) for f in csvs],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
        row["verify_rc"] = vr.returncode
        with open(os.path.join(run_dir, "verify.out"), "wb") as f:
            f.write(vr.stdout)
        if vr.returncode != 0:
            row["status"] = "verificacion_fallida"
            row["detail"] = vr.stdout.decode().strip().split("\n")[0]
        acc, ext = read_summary_totals(os.path.join(run_dir, "resumen.csv"))
        row["accepted"], row["extracted"] = acc, ext

    return row


def cleanup_segment(name):
    """Borra el objeto de memoria compartida que quedo de una corrida
    fallida. En Linux viven en /dev/shm; si no existe el directorio, no hay
    nada que hacer y el nombre se liberara al reiniciar."""
    path = os.path.join("/dev/shm", name.lstrip("/"))
    try:
        os.unlink(path)
    except OSError:
        pass


def read_summary_totals(path):
    """Saca accepted y extracted del resumen del finisher."""
    acc = ext = ""
    try:
        with open(path, newline="") as f:
            for rec in csv.DictReader(f):
                if rec.get("metric") == "accepted":
                    acc = rec.get("value", "")
                elif rec.get("metric") == "extracted":
                    ext = rec.get("value", "")
    except OSError:
        pass
    return acc, ext


# ---- Orquestacion --------------------------------------------------------

def build_matrix(quick):
    """Las 45 combinaciones, o 3 con --quick para probar el script."""
    caps = [8] if quick else CAPACITIES
    scen = [(5, 5)] if quick else SCENARIOS
    reps = 3 if quick else REPETITIONS
    out = []
    for cap in caps:
        for mp, mc in scen:
            for rep in range(1, reps + 1):
                out.append((cap, mp, mc, rep))
    return out


def main():
    ap = argparse.ArgumentParser(description="Corre el experimento del Proyecto 2.")
    ap.add_argument("--out", default="results", help="directorio de salida (por omision results/)")
    ap.add_argument("--base-seed", type=int, default=DEFAULT_BASE_SEED,
                    help="semilla base; la misma reproduce el experimento")
    ap.add_argument("--quick", action="store_true",
                    help="solo 3 corridas (capacidad 8, medias 5/5), para probar el script")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    missing = [b for b in BINARIES if not os.access(os.path.join(root, b), os.X_OK)]
    if missing:
        sys.exit(f"faltan ejecutables: {', '.join(missing)}. Corre `make` primero.")

    out_dir = args.out if os.path.isabs(args.out) else os.path.join(root, args.out)
    os.makedirs(out_dir, exist_ok=True)
    matrix = build_matrix(args.quick)

    manifest_path = os.path.join(out_dir, "manifiesto.csv")
    log(f"{len(matrix)} corridas, semilla base {args.base_seed}")
    log(f"salida en {out_dir}")

    rows = []
    started = time.monotonic()
    with open(manifest_path, "w", newline="") as mf:
        writer = csv.DictWriter(mf, fieldnames=MANIFEST_FIELDS)
        writer.writeheader()
        for index, (cap, mp, mc, rep) in enumerate(matrix, start=1):
            run_id = f"cap{cap}_mp{mp}_mc{mc}_rep{rep}"
            log(f"[{index}/{len(matrix)}] {run_id}")
            row = run_once(root, out_dir, run_id, cap, mp, mc, rep,
                           index, args.base_seed)
            rows.append(row)
            writer.writerow(row)
            mf.flush()          # el manifiesto queda util si se interrumpe
            mark = "ok" if row["status"] == "ok" else row["status"].upper()
            log(f"    {mark} en {row['wall_s']} s"
                + (f" — {row['detail']}" if row["detail"] else ""))

    ok = sum(1 for r in rows if r["status"] == "ok")
    log("")
    log(f"{ok}/{len(rows)} corridas validas en {time.monotonic() - started:.0f} s")
    log(f"manifiesto: {manifest_path}")
    bad = [r for r in rows if r["status"] != "ok"]
    if bad:
        log("corridas con problemas:")
        for r in bad:
            log(f"  {r['run_id']}: {r['status']} — {r['detail']}")
        return 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        signal.signal(signal.SIGINT, signal.SIG_DFL)
        sys.exit(130)
