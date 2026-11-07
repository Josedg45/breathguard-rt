#!/usr/bin/env python3
"""m3_pulse_summary.py - resumen de las corridas con Pulse (results/m3/{sim,ctrl,pat}_<etiqueta>.csv[.gz]).

Por corrida: ciclos del controlador y tardíos, pasos de Pulse > 20 ms, paso máximo, atraso máximo,
ventanas perdidas, correcciones de volumen |dV| (desde la ventana 5), Vt, y signos vitales al final.
Uso: m3_pulse_summary.py etiqueta [etiqueta...]
"""
import csv
import gzip
import os
import sys


def load(path):
    if not os.path.exists(path) and os.path.exists(path + ".gz"):
        path += ".gz"
    with (gzip.open(path, "rt") if path.endswith(".gz") else open(path)) as f:
        rows = list(csv.DictReader(f))
    return {k: [float(r[k]) for r in rows] for k in rows[0]}


def pct(sorted_vals, p):
    return sorted_vals[min(len(sorted_vals) - 1, int(len(sorted_vals) * p))]


print("corrida      ciclos  tardíos  Pulse>20ms  paso_máx  atraso_máx  perdidas  |dV| p50/p99/máx (mL)   "
      "Vt   SpO2   EtCO2  FC")
for lab in sys.argv[1:]:
    c = load(f"results/m3/ctrl_{lab}.csv")
    p = load(f"results/m3/pat_{lab}.csv")
    s = load(f"results/m3/sim_{lab}.csv")
    late = sum(1 for x in c["period_ms"][1:] if x > 1.5)
    steps = p["step_us"]
    dv = sorted(abs(x) for x in p["last_dv_ml"][5:])
    # Vt: por respiración desde el inicio del controlador; régimen = de la 3.ª en adelante.
    t0, period, ti = c["t_s"][0], 5.0, 5.0 / 3
    t, v = s["t_s"], s["v_l"]
    vts = []
    b = 0
    i = 0
    while t0 + (b + 1) * period <= t[-1]:
        st, en = t0 + b * period, t0 + b * period + ti
        while i < len(t) and t[i] < st:
            i += 1
        j = i
        vmax = v[i]
        while j < len(t) and t[j] < en:
            vmax = max(vmax, v[j])
            j += 1
        vts.append((vmax - v[i]) * 1000)
        b += 1
    vt = sum(vts[2:]) / len(vts[2:])
    print(f"{lab:12s} {len(c['t_s']):7d} {late:7d}  {sum(1 for x in steps if x > 20000):8d}   "
          f"{max(steps) / 1000:6.1f} ms  {int(max(p['backlog'])):6d}     {int(p['lost'][-1]):5d}    "
          f"{pct(dv, 0.5):.3f}/{pct(dv, 0.99):.3f}/{dv[-1]:.3f}      {vt:4.0f}  {p['spo2'][-1] * 100:.1f}  "
          f"{p['etco2'][-1]:5.1f}  {p['hr'][-1]:3.0f}")
