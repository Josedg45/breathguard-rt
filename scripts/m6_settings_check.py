#!/usr/bin/env python3
"""m6_settings_check.py - escena 'ajustes' de M6: tabla por respiracion (inicio, periodo, Ppico, PEEP al final de
la espiracion, Vt, quien mando) a partir de la planta, con los eventos de safety_mon intercalados.
Uso: m6_settings_check.py etiqueta   (lee results/m6/sim_<etiqueta>.csv[.gz] y ev_<etiqueta>.csv)
"""
import csv
import gzip
import os
import sys

L = sys.argv[1]
D = "results/m6"


def load(name):
    p = os.path.join(D, name)
    if os.path.exists(p + ".gz"):
        return list(csv.DictReader(gzip.open(p + ".gz", "rt")))
    return list(csv.DictReader(open(p)))


sim = load(f"sim_{L}.csv")
ev = load(f"ev_{L}.csv")
t0 = float(ev[0]["t_s"])  # arranque de safety_mon
t = [float(r["t_s"]) - t0 for r in sim]
paw = [float(r["paw"]) for r in sim]
v = [float(r["v_l"]) for r in sim]
blo = [float(r["blower"]) for r in sim]
src = [int(r["src"]) for r in sim]
exh = [float(r["exh_open"]) for r in sim]
# Inicio de respiracion: la valvula espiratoria se cierra (exh_open pasa de > 0.5 a < 0.5).
starts = [i for i in range(1, len(sim)) if exh[i - 1] > 0.5 >= exh[i] and t[i] > 0]
evs = [(float(e["t_s"]) - t0, e["evento"], e["detalle"]) for e in ev]
print(f"{'t_ini':>7} {'periodo':>7} {'Ppico':>6} {'PEEP':>5} {'Vt mL':>6}  manda")
ei = 0
for a, b in zip(starts, starts[1:]):
    while ei < len(evs) and evs[ei][0] < t[a]:
        print(f"        >> {evs[ei][0]:7.3f} {evs[ei][1]} {evs[ei][2]}")
        ei += 1
    seg = range(a, b)
    who = sum(src[i] for i in seg) / len(seg)
    print(f"{t[a]:7.3f} {t[b] - t[a]:7.3f} {max(paw[i] for i in seg):6.1f} {paw[b - 1]:5.1f} "
          f"{(max(v[i] for i in seg) - v[a]) * 1e3:6.0f}  {'respaldo' if who > 0.5 else 'breath_ctrl'}"
          f"{'' if who in (0, 1) else f' ({who * 100:.0f} % respaldo)'}")
