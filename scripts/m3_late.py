#!/usr/bin/env python3
"""m3_late.py - qué le pasa a la presión cuando breath_ctrl llega tarde.

Para cada corrida: Vt por respiración (mín/máx), ciclos tardíos, y el peor desvío de Paw real
respecto al objetivo dentro de los huecos del controlador (comparado con el desvío normal).
Uso: m3_late.py etiqueta [etiqueta...]   (lee results/m3/sim_<e>.csv y ctrl_<e>.csv)
"""
import bisect
import csv
import gzip
import os
import sys


def load(p):
    if not os.path.exists(p) and os.path.exists(p + ".gz"):
        p += ".gz"
    with (gzip.open(p, "rt") if p.endswith(".gz") else open(p)) as f:
        r = list(csv.DictReader(f))
    return {k: [float(x[k]) for x in r] for k in r[0]}


print("corrida     ciclos  tarde>1.5ms  hueco_máx  tardíos_en_subida  peor_desvío_en_hueco  desvío_p99_normal")
for lab in sys.argv[1:]:
    s, c = load(f"results/m3/sim_{lab}.csv"), load(f"results/m3/ctrl_{lab}.csv")
    ts, paw = s["t_s"], s["paw"]
    tc = c["t_s"]
    worst, worst_info, normal = 0.0, "", []
    late_rise = 0
    t0 = tc[0]
    for i in range(1, len(tc)):
        a, b = tc[i - 1], tc[i]
        tgt = c["target"][i - 1]
        tb = (a - t0) % 5.0
        j0, j1 = bisect.bisect_right(ts, a), bisect.bisect_right(ts, b)
        if j1 <= j0:
            continue
        dev = max(abs(paw[j] - tgt) for j in range(j0, j1))
        if c["phase"][i - 1] == 0 and tb > 0.3:  # meseta inspiratoria
            if b - a > 0.0015:
                if dev > worst:
                    worst, worst_info = dev, f"{dev:.2f} cmH2O en hueco de {(b - a) * 1e3:.1f} ms"
            else:
                normal.append(dev)
        if c["phase"][i - 1] == 0 and tb < 0.15 and b - a > 0.0015:
            late_rise += 1
    normal.sort()
    late = sum(1 for p in c["period_ms"][1:] if p > 1.5)
    print(f"{lab:10s} {len(tc):7d}  {late:6d}       {max(c['period_ms'][1:]):6.2f} ms  {late_rise:6d}"
          f"            {worst_info:28s}  {normal[int(len(normal) * 0.99)]:.2f} cmH2O")
