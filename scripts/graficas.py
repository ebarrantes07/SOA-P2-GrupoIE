#!/usr/bin/env python3
"""Graficas del experimento (Issue #25, decisiones en D33).

Lee results/seleccionados/agregados.csv (lo escribe scripts/analisis.py) y
deja tres figuras SVG en la misma carpeta:

    fig1_throughput.svg   throughput vs capacidad, por escenario, con sd
    fig2_latencia.svg     latencia media y p95 vs capacidad (eje log)
    fig3_esperas.svg      espera del productor y del consumidor, en % de vida

    python3 scripts/graficas.py
    python3 scripts/graficas.py --agregados otra/ruta.csv --out otra/carpeta

SVG a mano y sin dependencias: el README declara que los scripts usan solo
la biblioteca estandar, y una figura para un informe impreso no necesita
nada mas. Las decisiones de forma y color estan en D33.
"""
import argparse
import csv
import os
import sys

# ---- Paleta -------------------------------------------------------------

# Slots 1 a 3 de la paleta categorica de referencia, usados sin tocar: es la
# terna documentada como valida en todos los pares para modo claro (los
# small multiples de la figura 3 exigen justamente esa garantia). El
# validador del metodo necesita node, que no esta en este entorno, asi que
# en vez de inventar hexadecimales se reutilizan los ya validados.
SERIES = ["#2a78d6", "#eb6834", "#1baf7a"]      # azul, naranja, aqua
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_SOFT = "#52514e"
GRID = "#e5e4e0"
AXIS = "#b9b8b2"

FONT = ("-apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, "
        "'Helvetica Neue', Arial, sans-serif")

# El color sigue al escenario en las tres figuras, no al rango ni al rol:
# un mismo color significa lo mismo en todas.
SCENARIOS = [(2, 8), (5, 5), (8, 2)]
CAPACITIES = [1, 8, 64]


def scenario_label(mp, mc):
    return f"productor {mp} ms / consumidor {mc} ms"


# ---- Utilidades de SVG --------------------------------------------------

def esc(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


class Svg:
    """Acumulador de elementos SVG. Sin dependencias y sin estado oculto."""

    def __init__(self, width, height):
        self.w = width
        self.h = height
        self.parts = []

    def add(self, markup):
        self.parts.append(markup)

    def text(self, x, y, s, size=11, fill=INK_SOFT, anchor="start", weight="normal"):
        self.add(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{fill}" '
                 f'text-anchor="{anchor}" font-weight="{weight}">{esc(s)}</text>')

    def line(self, x1, y1, x2, y2, stroke=GRID, width=1, dash=None):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        self.add(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
                 f'stroke="{stroke}" stroke-width="{width}"{d}/>')

    def bar(self, x, y, w, h, fill, radius=4):
        """Barra con las dos esquinas de la punta redondeadas, anclada a la
        base: la punta es el dato, asi que se redondea solo ese extremo."""
        r = min(radius, w / 2, h) if h > 0 else 0
        self.add(f'<path d="M{x:.1f},{y + h:.1f} L{x:.1f},{y + r:.1f} '
                 f'Q{x:.1f},{y:.1f} {x + r:.1f},{y:.1f} L{x + w - r:.1f},{y:.1f} '
                 f'Q{x + w:.1f},{y:.1f} {x + w:.1f},{y + r:.1f} '
                 f'L{x + w:.1f},{y + h:.1f} Z" fill="{fill}"/>')

    def circle(self, cx, cy, r, fill):
        self.add(f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r}" fill="{fill}"/>')

    def square(self, cx, cy, half, stroke):
        self.add(f'<rect x="{cx - half:.1f}" y="{cy - half:.1f}" width="{2 * half}" '
                 f'height="{2 * half}" fill="{SURFACE}" stroke="{stroke}" '
                 f'stroke-width="1.6"/>')

    def polyline(self, points, stroke, width=2, dash=None):
        pts = " ".join(f"{x:.1f},{y:.1f}" for x, y in points)
        d = f' stroke-dasharray="{dash}"' if dash else ""
        self.add(f'<polyline points="{pts}" fill="none" stroke="{stroke}" '
                 f'stroke-width="{width}" stroke-linecap="round"{d}/>')

    def render(self):
        body = "\n  ".join(self.parts)
        return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" '
                f'height="{self.h}" viewBox="0 0 {self.w} {self.h}" '
                f'font-family="{FONT}">\n'
                f'  <rect width="{self.w}" height="{self.h}" fill="{SURFACE}"/>\n'
                f'  {body}\n</svg>\n')


def nice_max(value, ticks=5):
    """Tope de eje redondo y >= value, para que las marcas no queden feas."""
    if value <= 0:
        return 1.0, [0.0]
    import math
    raw = value / ticks
    mag = 10 ** math.floor(math.log10(raw))
    for mult in (1, 2, 2.5, 5, 10):
        step = mult * mag
        if step >= raw:
            break
    top = step * ticks
    while top < value:
        top += step
    return top, [i * step for i in range(int(round(top / step)) + 1)]


# ---- Datos --------------------------------------------------------------

def load_aggregates(path):
    """{(capacidad, media_prod, media_cons): {metrica: (media, sd)}}"""
    if not os.path.exists(path):
        print(f"graficas: falta {path}; corra antes scripts/analisis.py",
              file=sys.stderr)
        sys.exit(1)
    data = {}
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            key = (int(row["capacity"]), int(row["mean_prod_ms"]), int(row["mean_cons_ms"]))
            data.setdefault(key, {})[row["metric"]] = (float(row["mean"]), float(row["sd"]))
    missing = [k for k in ((c, mp, mc) for c in CAPACITIES for mp, mc in SCENARIOS)
               if k not in data]
    if missing:
        print(f"graficas: faltan combinaciones en el agregado: {missing}", file=sys.stderr)
        sys.exit(1)
    return data


def legend_row(svg, x, y, labels, colors, size=11):
    """Leyenda siempre presente: con tres series el color no alcanza solo."""
    for label, color in zip(labels, colors):
        svg.add(f'<rect x="{x:.1f}" y="{y - 8:.1f}" width="10" height="10" rx="2" '
                f'fill="{color}"/>')
        svg.text(x + 15, y, label, size=size, fill=INK_SOFT)
        x += 16 + 6.3 * len(label)
    return x


# ---- Figura 1: throughput ----------------------------------------------

def fig_throughput(data, out_dir):
    w, h = 820, 418
    left, right, top, bottom = 72, 24, 84, 92
    svg = Svg(w, h)
    plot_w = w - left - right
    plot_h = h - top - bottom

    svg.text(left - 2, 28, "Throughput por capacidad y escenario", size=15,
             fill=INK, weight="600")
    svg.text(left - 2, 46, "media de 5 repeticiones; la barra de error es la "
                           "desviacion estandar entre repeticiones", size=11)

    top_val, ticks = nice_max(max(data[(c, mp, mc)]["throughput"][0]
                                  + data[(c, mp, mc)]["throughput"][1]
                                  for c in CAPACITIES for mp, mc in SCENARIOS))
    for t in ticks:
        y = top + plot_h - plot_h * t / top_val
        svg.line(left, y, left + plot_w, y, stroke=GRID)
        svg.text(left - 8, y + 3.5, f"{t:g}", anchor="end")
    svg.text(left - 2, top - 12, "mensajes por segundo", size=11)
    svg.line(left, top + plot_h, left + plot_w, top + plot_h, stroke=AXIS)

    group_w = plot_w / len(CAPACITIES)
    bar_w = (group_w - 46 - 2 * 2) / 3          # 2 px de aire entre barras
    for gi, cap in enumerate(CAPACITIES):
        gx = left + gi * group_w + 23
        for si, (mp, mc) in enumerate(SCENARIOS):
            mean, sd = data[(cap, mp, mc)]["throughput"]
            x = gx + si * (bar_w + 2)
            bh = plot_h * mean / top_val
            y = top + plot_h - bh
            svg.bar(x, y, bar_w, bh, SERIES[si])
            # Etiqueta directa: la regla de relieve pide valores visibles
            # porque uno de los tres colores no llega a 3:1 sobre el fondo.
            svg.text(x + bar_w / 2, y - 14, f"{mean:.0f}", size=11, fill=INK,
                     anchor="middle", weight="600")
            cx = x + bar_w / 2
            e_top = top + plot_h - plot_h * (mean + sd) / top_val
            e_bot = top + plot_h - plot_h * max(mean - sd, 0) / top_val
            svg.line(cx, e_top, cx, e_bot, stroke=INK, width=1.2)
            svg.line(cx - 4, e_top, cx + 4, e_top, stroke=INK, width=1.2)
            svg.line(cx - 4, e_bot, cx + 4, e_bot, stroke=INK, width=1.2)
        svg.text(left + gi * group_w + group_w / 2, top + plot_h + 20,
                 f"capacidad {cap}", size=12, fill=INK, anchor="middle")

    legend_row(svg, left, h - 34, [scenario_label(*s) for s in SCENARIOS], SERIES)
    path = os.path.join(out_dir, "fig1_throughput.svg")
    with open(path, "w", newline="\n") as f:
        f.write(svg.render())
    return path


# ---- Figura 2: latencia (eje logaritmico) -------------------------------

def fig_latencia(data, out_dir):
    import math
    w, h = 820, 448
    left, right, top, bottom = 72, 24, 84, 108
    svg = Svg(w, h)
    plot_w = w - left - right
    plot_h = h - top - bottom

    svg.text(left - 2, 28, "Latencia insercion a extraccion por capacidad", size=15,
             fill=INK, weight="600")
    svg.text(left - 2, 46, "circulo lleno: media; cuadro hueco: percentil 95 "
                           "(aproximado por histograma, error <= 6.25 %)", size=11)

    lo, hi = 0.03, 300.0                        # ms; cubre de 0.06 a 160
    def ypos(value):
        v = min(max(value, lo), hi)
        return top + plot_h - plot_h * (math.log10(v) - math.log10(lo)) / (
            math.log10(hi) - math.log10(lo))

    for t in (0.03, 0.1, 0.3, 1, 3, 10, 30, 100, 300):
        y = ypos(t)
        svg.line(left, y, left + plot_w, y, stroke=GRID)
        svg.text(left - 8, y + 3.5, f"{t:g}", anchor="end")
    svg.text(left - 2, top - 12, "milisegundos (escala logaritmica)", size=11)
    svg.line(left, top + plot_h, left + plot_w, top + plot_h, stroke=AXIS)

    step = plot_w / len(CAPACITIES)
    xs = [left + step * (i + 0.5) for i in range(len(CAPACITIES))]
    for si, (mp, mc) in enumerate(SCENARIOS):
        dx = (si - 1) * 14                      # separacion para que no se tapen
        means = [(xs[i] + dx, ypos(data[(c, mp, mc)]["latency_mean"][0]))
                 for i, c in enumerate(CAPACITIES)]
        p95s = [(xs[i] + dx, ypos(data[(c, mp, mc)]["latency_p95"][0]))
                for i, c in enumerate(CAPACITIES)]
        svg.polyline(p95s, SERIES[si], width=1.6, dash="5 4")
        svg.polyline(means, SERIES[si], width=2)
        for (x, y), (px, py) in zip(means, p95s):
            svg.line(x, y, px, py, stroke=SERIES[si], width=1)
            svg.square(px, py, 4.5, SERIES[si])
            svg.circle(x, y, 4.5, SERIES[si])
    # Etiqueta directa solo en los extremos de cada serie, para no saturar.
    for si, (mp, mc) in enumerate(SCENARIOS):
        dx = (si - 1) * 14
        for i in (0, len(CAPACITIES) - 1):
            mean = data[(CAPACITIES[i], mp, mc)]["latency_mean"][0]
            label = f"{mean:.2f}" if mean < 10 else f"{mean:.0f}"
            svg.text(xs[i] + dx, ypos(mean) + 18, label, size=10, fill=INK,
                     anchor="middle", weight="600")
    for i, cap in enumerate(CAPACITIES):
        svg.text(xs[i], top + plot_h + 20, f"capacidad {cap}", size=12, fill=INK,
                 anchor="middle")

    legend_row(svg, left, h - 50, [scenario_label(*s) for s in SCENARIOS], SERIES)
    svg.circle(left + 5, h - 26, 4.5, INK_SOFT)
    svg.text(left + 15, h - 22, "media", size=11)
    svg.square(left + 85, h - 26, 4.5, INK_SOFT)
    svg.text(left + 95, h - 22, "percentil 95", size=11)
    path = os.path.join(out_dir, "fig2_latencia.svg")
    with open(path, "w", newline="\n") as f:
        f.write(svg.render())
    return path


# ---- Figura 3: esperas (small multiples) --------------------------------

def fig_esperas(data, out_dir):
    w, h = 860, 420
    left, right, top, bottom = 72, 24, 76, 92
    gap = 56
    svg = Svg(w, h)
    panel_w = (w - left - right - gap) / 2
    plot_h = h - top - bottom

    svg.text(left - 2, 28, "Donde se espera: productor y consumidor", size=15,
             fill=INK, weight="600")
    svg.text(left - 2, 46, "espera acumulada del rol como porcentaje de su "
                           "tiempo de vida; misma escala en los dos paneles", size=11)

    top_val, ticks = nice_max(100.0, 5)
    panels = [("Productor, esperando espacio (not_full)", "wait_space_frac"),
              ("Consumidor, esperando datos (not_empty)", "wait_data_frac")]
    for pi, (title, metric) in enumerate(panels):
        px = left + pi * (panel_w + gap)
        svg.text(px, top - 16, title, size=12, fill=INK, weight="600")
        for t in ticks:
            y = top + plot_h - plot_h * t / top_val
            svg.line(px, y, px + panel_w, y, stroke=GRID)
            svg.text(px - 8, y + 3.5, f"{t:g}%", anchor="end")
        svg.line(px, top + plot_h, px + panel_w, top + plot_h, stroke=AXIS)

        group_w = panel_w / len(CAPACITIES)
        bar_w = (group_w - 26 - 2 * 2) / 3
        for gi, cap in enumerate(CAPACITIES):
            gx = px + gi * group_w + 13
            for si, (mp, mc) in enumerate(SCENARIOS):
                mean, _sd = data[(cap, mp, mc)][metric]
                x = gx + si * (bar_w + 2)
                bh = plot_h * mean / top_val
                y = top + plot_h - bh
                svg.bar(x, y, bar_w, bh, SERIES[si])
                svg.text(x + bar_w / 2, y - 6, f"{mean:.0f}", size=10, fill=INK,
                         anchor="middle", weight="600")
            svg.text(px + gi * group_w + group_w / 2, top + plot_h + 20,
                     f"cap {cap}", size=11, fill=INK, anchor="middle")

    legend_row(svg, left, h - 34, [scenario_label(*s) for s in SCENARIOS], SERIES)
    path = os.path.join(out_dir, "fig3_esperas.svg")
    with open(path, "w", newline="\n") as f:
        f.write(svg.render())
    return path


def main():
    ap = argparse.ArgumentParser(description="Graficas del experimento (#25)")
    default_dir = os.path.join("results", "seleccionados")
    ap.add_argument("--agregados", default=os.path.join(default_dir, "agregados.csv"))
    ap.add_argument("--out", default=default_dir)
    args = ap.parse_args()

    data = load_aggregates(args.agregados)
    os.makedirs(args.out, exist_ok=True)
    for path in (fig_throughput(data, args.out), fig_latencia(data, args.out),
                 fig_esperas(data, args.out)):
        print(f"figura: {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
