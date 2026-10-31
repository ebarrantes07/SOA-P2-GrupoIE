#!/usr/bin/env python3
"""Modelo de referencia del productor-consumidor (Issue #25, decisiones en D32).

Simulacion de eventos discretos de lo que miden las tres figuras: 4+4
procesos, pausas exponenciales, buffer de capacidad C. El modelo tiene
encolamiento y bloqueo, pero NO tiene mutex, ni CSV, ni llamadas al
sistema: el traspaso a un proceso dormido es instantaneo.

Para que sirve. Una medicion sola no distingue "esto lo causa el regimen
productor-consumidor" de "esto lo causa nuestra implementacion". El modelo
separa las dos: lo que reproduce es del regimen, y lo que le falta para
llegar a lo medido es costo real de la implementacion o del sistema
operativo. Con eso se justifica, por ejemplo, que la columna 8/2 de la
figura de latencia no mida el buffer (seccion 5 del analisis).

    python3 scripts/modelo.py                 # las cuatro tablas
    python3 scripts/modelo.py --reps 10       # mas rapido, mas ruidoso

Solo biblioteca estandar, como el resto de scripts/ (ver README).
"""
import argparse
import heapq
import math
import random
import statistics
from collections import deque

PRODUCERS = CONSUMERS = 4
MESSAGES_PER_PRODUCER = 200
SCENARIOS = [(2, 8), (5, 5), (8, 2)]
CAPACITIES = [1, 8, 64]

# Lo medido en results/seleccionados/agregados.csv, para comparar al lado.
# throughput msgs/s, latencia media ms, espera productor %, espera consumidor %
MEASURED = {
    (2, 8): {1: (488.1, 1.954, 73.3, 1.7), 8: (492.4, 15.992, 73.4, 0.5),
             64: (473.2, 128.047, 71.2, 0.3)},
    (5, 5): {1: (611.4, 0.774, 15.8, 20.9), 8: (695.4, 4.773, 6.1, 13.3),
             64: (721.5, 24.617, 0.6, 9.0)},
    (8, 2): {1: (459.5, 0.079, 0.1, 75.3), 8: (477.0, 0.078, 0.0, 74.8),
             64: (445.4, 0.062, 0.0, 77.1)},
}


def simulate(cap, mean_prod, mean_cons, msgs=MESSAGES_PER_PRODUCER,
             cut_delay_ms=3.0, seed=0):
    """Una corrida. Devuelve un dict con lo que miden las tres figuras.

    `cut_delay_ms` es el retardo del corte: experimento.py lanza el finisher
    cuando terminan TODOS los productores, asi que el consumidor sigue
    dormido en not_empty desde su ultima extraccion hasta el corte, y ese
    tiempo entra en su espera (igual que en el sistema real).
    """
    rng = random.Random(seed)
    buf = deque()                       # instante de insercion de cada mensaje
    produced = [0] * PRODUCERS
    done_at = [None] * PRODUCERS        # cuando cada productor cumple su cuota
    target = PRODUCERS * msgs

    prod_blocked_since, cons_blocked_since = {}, {}
    prod_wait = [0.0] * PRODUCERS
    cons_wait = [0.0] * CONSUMERS
    cons_ready = [0.0] * CONSUMERS      # fin de su procesamiento simulado
    prod_queue, cons_queue = [], []     # bloqueados, en orden de llegada

    lats = []
    level_time = {}                     # nivel del buffer -> tiempo acumulado
    t = t_prev = 0.0
    last_dequeue = 0.0

    heap = []
    for i in range(PRODUCERS):          # la pausa va ANTES del primer put
        heapq.heappush(heap, (rng.expovariate(1.0 / mean_prod), 0, i))
    for i in range(CONSUMERS):
        heapq.heappush(heap, (0.0, 1, i))

    def schedule_next_put(i, now):
        if produced[i] < msgs:
            heapq.heappush(heap, (now + rng.expovariate(1.0 / mean_prod), 0, i))
        else:
            done_at[i] = now

    def consume(c, now):
        """El consumidor c extrae el mas viejo y se pone a procesar."""
        lats.append(now - buf.popleft())
        dur = rng.expovariate(1.0 / mean_cons)
        cons_ready[c] = now + dur
        heapq.heappush(heap, (now + dur, 1, c))

    while len(lats) < target and heap:
        t, kind, i = heapq.heappop(heap)
        if t > t_prev:                  # contabilizar el nivel que acaba de dejarse
            level_time[len(buf)] = level_time.get(len(buf), 0.0) + (t - t_prev)
            t_prev = t

        if kind == 0:                                   # productor quiere insertar
            if produced[i] >= msgs:
                continue
            if len(buf) < cap:
                buf.append(t)
                produced[i] += 1
                if cons_queue:                          # habia alguien esperando datos
                    c = cons_queue.pop(0)
                    cons_wait[c] += t - cons_blocked_since.pop(c)
                    consume(c, t)
                    last_dequeue = t
                schedule_next_put(i, t)
            else:                                       # buffer lleno
                prod_queue.append(i)
                prod_blocked_since[i] = t
        else:                                           # consumidor quiere extraer
            if buf:
                consume(i, t)
                last_dequeue = t
                if prod_queue:                          # se libero una casilla
                    p = prod_queue.pop(0)
                    prod_wait[p] += t - prod_blocked_since.pop(p)
                    buf.append(t)
                    produced[p] += 1
                    schedule_next_put(p, t)
            else:                                       # buffer vacio
                cons_queue.append(i)
                cons_blocked_since[i] = t

    # El corte y el retiro de los consumidores.
    cut = max(x for x in done_at if x is not None) + cut_delay_ms
    leave = max(cut, last_dequeue, max(cons_ready))
    for c in range(CONSUMERS):
        if c in cons_blocked_since:                     # ya estaba dormido
            cons_wait[c] += leave - cons_blocked_since[c]
        else:                                           # termina y se duerme
            cons_wait[c] += max(0.0, leave - cons_ready[c])

    lats.sort()
    total = sum(level_time.values())
    return {
        "throughput": target / (last_dequeue / 1000.0),
        "lat_mean": statistics.mean(lats),
        "lat_p95": lats[int(0.95 * len(lats))],
        "lat_max": lats[-1],
        "prod_frac": statistics.mean(100 * prod_wait[i] / done_at[i]
                                     for i in range(PRODUCERS)),
        "cons_frac": statistics.mean(100 * cons_wait[i] / leave
                                     for i in range(CONSUMERS)),
        "empty_frac": 100 * level_time.get(0, 0.0) / total,
        "full_frac": 100 * level_time.get(cap, 0.0) / total,
        "occupancy": sum(k * v for k, v in level_time.items()) / total,
        "tail_frac": 100 * max(0.0, leave - last_dequeue) / leave,
    }


def avg(cap, mp, mc, reps, key, **kw):
    return statistics.mean(simulate(cap, mp, mc, seed=s, **kw)[key]
                           for s in range(reps))


def table_validacion(reps):
    print("== 1. Modelo contra medicion (lo que reproduce es del regimen) ==")
    print(f"{'celda':<13}{'thr mod':>9}{'thr med':>9}{'lat mod':>9}{'lat med':>9}"
          f"{'prod mod':>10}{'prod med':>10}{'cons mod':>10}{'cons med':>10}")
    for mp, mc in SCENARIOS:
        for cap in CAPACITIES:
            thr, lat, pf, cf = MEASURED[(mp, mc)][cap]
            print(f"{f'{mp}/{mc} cap {cap}':<13}"
                  f"{avg(cap, mp, mc, reps, 'throughput'):>9.1f}{thr:>9.1f}"
                  f"{avg(cap, mp, mc, reps, 'lat_mean'):>9.3f}{lat:>9.3f}"
                  f"{avg(cap, mp, mc, reps, 'prod_frac'):>9.1f}%{pf:>9.1f}%"
                  f"{avg(cap, mp, mc, reps, 'cons_frac'):>9.1f}%{cf:>9.1f}%")
    print()


def table_plateau(reps):
    print("== 2. Barrido de capacidad: donde deja de mejorar el throughput ==")
    caps = [1, 2, 4, 8, 16, 32, 64, 128, 256, 1024, 10 ** 7]
    for mp, mc in SCENARIOS:
        ceiling = min(4000 / mp, 4000 / mc)
        print(f"  {mp}/{mc} ms (techo {ceiling:.0f}/s)")
        prev = None
        for cap in caps:
            m = avg(cap, mp, mc, reps, "throughput")
            delta = "" if prev is None else f"  delta {m - prev:+6.2f}"
            label = "inf" if cap == 10 ** 7 else str(cap)
            print(f"    cap {label:>7}: {m:>6.1f}/s  {100 * m / ceiling:>5.1f}% del techo{delta}")
            prev = m
    print()


def table_mensajes(reps):
    print("== 3. Deficit residual (capacidad infinita) contra mensajes, 5/5 ==")
    print(f"{'N/prod':>8}{'thr':>9}{'% techo':>9}{'deficit':>9}{'1.03/raiz(N)':>14}")
    ns = [25, 50, 100, 200, 400, 800, 1600, 3200]
    deficits = []
    for n in ns:
        m = avg(10 ** 7, 5, 5, reps, "throughput", msgs=n)
        deficit = 100 - 100 * m / 800
        deficits.append(deficit)
        print(f"{n:>8}{m:>9.1f}{100 * m / 800:>8.1f}%{deficit:>8.2f}%"
              f"{100 * 1.029 / math.sqrt(n):>13.2f}%")
    lx = [math.log(n) for n in ns]
    ly = [math.log(d) for d in deficits]
    mx, my = statistics.mean(lx), statistics.mean(ly)
    b = (sum((lx[i] - mx) * (ly[i] - my) for i in range(len(ns)))
         / sum((lx[i] - mx) ** 2 for i in range(len(ns))))
    print(f"  ajuste  deficit ~ N^b  ->  b = {b:.3f}   (la ley 1/raiz(N) predice -0.500)")
    print()


def table_ocupacion(reps):
    print("== 4. Nivel del buffer: solo se puede bloquear en un borde ==")
    print(f"{'celda':<13}{'vacio n=0':>11}{'lleno n=C':>11}{'zona media':>12}{'ocup media':>12}")
    for mp, mc in SCENARIOS:
        for cap in ([1, 2, 4, 8, 16, 32, 64] if (mp, mc) == (5, 5) else CAPACITIES):
            e = avg(cap, mp, mc, reps, "empty_frac")
            f = avg(cap, mp, mc, reps, "full_frac")
            print(f"{f'{mp}/{mc} cap {cap}':<13}{e:>10.1f}%{f:>10.1f}%"
                  f"{max(0.0, 100 - e - f):>11.1f}%{avg(cap, mp, mc, reps, 'occupancy'):>12.2f}")
    print()


def table_corte(reps):
    print("== 5. Cuanto infla la espera del consumidor un corte tardio (8/2 cap 8) ==")
    print(f"{'retardo':>10}{'cons frac':>11}{'ventana tras la ult. extraccion':>34}")
    for delay in (0.0, 3.0, 10.0, 50.0, 200.0, 1000.0):
        c = avg(8, 8, 2, reps, "cons_frac", cut_delay_ms=delay)
        tail = avg(8, 8, 2, reps, "tail_frac", cut_delay_ms=delay)
        print(f"{delay:>7.0f} ms{c:>10.1f}%{tail:>32.2f}%")
    print()


def main():
    ap = argparse.ArgumentParser(description="Modelo de referencia del experimento (#25)")
    ap.add_argument("--reps", type=int, default=40,
                    help="corridas por celda (default 40)")
    args = ap.parse_args()
    table_validacion(args.reps)
    table_plateau(args.reps)
    table_mensajes(args.reps)
    table_ocupacion(args.reps)
    table_corte(args.reps)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
