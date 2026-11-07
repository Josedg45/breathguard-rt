#!/usr/bin/env python3
"""m3_analyze.py - métricas por respiración de un CSV de M3 (offline o de pneumatic_sim en QNX).

Columnas necesarias: t_s, paw, v_l. Con target y phase calcula también el error de seguimiento.
Uso: m3_analyze.py archivo.csv p_insp peep rr [--plot salida.png]
"""
import argparse
import csv
import math


def load(path):
    with open(path) as f:
        rows = list(csv.DictReader(f))
    return {k: [float(r[k]) for r in rows] for k in rows[0]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("p_insp", type=float)
    ap.add_argument("peep", type=float)
    ap.add_argument("rr", type=float)
    ap.add_argument("--ie", type=float, default=2.0)
    ap.add_argument("--r", type=float, default=22.4, help="R del paciente, para la fórmula")
    ap.add_argument("--c", type=float, default=0.058, help="C del paciente (L/cmH2O)")
    ap.add_argument("--t0", type=float, help="inicio de la 1.ª respiración (por defecto, 1.ª fila)")
    ap.add_argument("--ctrl", help="CSV de breath_ctrl: toma t0 de su 1.ª fila")
    ap.add_argument("--plot")
    a = ap.parse_args()

    d = load(a.csv)
    t, paw, v = d["t_s"], d["paw"], d["v_l"]
    t0 = t[0]
    if a.ctrl:
        t0 = load(a.ctrl)["t_s"][0]
    elif a.t0 is not None:
        t0 = a.t0
    period = 60.0 / a.rr
    ti = period / (1 + a.ie)

    # Fórmula del RC con presión cuadrada (APRENDIZAJE M2, paso 5).
    tau = a.r * a.c
    dp = a.p_insp - a.peep
    ei, ee, et = (math.exp(-x / tau) for x in (ti, period - ti, period))
    vt_formula = a.c * dp * (1 - ei) * (1 - ee) / (1 - et) * 1000

    nb = int((t[-1] - t0) / period)
    print(f"Fórmula RC (presión cuadrada): Vt = {vt_formula:.0f} mL")
    print("resp  Vt(mL)  PIP   PEEPmed  err_meseta_RMS")
    stats = []
    for b in range(nb):
        s, e = t0 + b * period, t0 + (b + 1) * period
        idx = [i for i in range(len(t)) if s <= t[i] < e]
        ins = [i for i in idx if t[i] - s < ti]
        vt = (max(v[i] for i in ins) - v[ins[0]]) * 1000
        pip = max(paw[i] for i in ins)
        last = [i for i in idx if t[i] > e - 0.1]
        peep_m = sum(paw[i] for i in last) / len(last)
        err = float("nan")
        if "target" in d:
            plateau = [i for i in ins if t[i] - s > 0.3]
            err = math.sqrt(sum((d["target"][i] - paw[i]) ** 2 for i in plateau) / len(plateau))
        stats.append((vt, pip, peep_m, err))
        print(f"{b:4d}  {vt:6.0f}  {pip:5.1f}  {peep_m:5.2f}  {err:6.3f}")

    steady = stats[2:] if len(stats) > 3 else stats
    vt_ss = sum(s[0] for s in steady) / len(steady)
    print(f"\nRégimen (resp. 2 en adelante): Vt {vt_ss:.0f} mL "
          f"({(vt_ss - vt_formula) / vt_formula * 100:+.1f} % contra la fórmula), "
          f"PIP máx {max(s[1] for s in steady):.1f}, PEEP {sum(s[2] for s in steady) / len(steady):.2f}")

    if a.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(3, 1, sharex=True, figsize=(10, 7))
        if "paw_meas" in d:
            ax[0].plot(t, d["paw_meas"], lw=0.5, alpha=0.5, label="Paw medida")
        ax[0].plot(t, paw, lw=1, label="Paw real")
        if a.ctrl:
            c = load(a.ctrl)
            ax[0].plot(c["t_s"], c["target"], "--", lw=1, label="objetivo")
        if "target" in d:
            ax[0].plot(t, d["target"], "--", lw=1, label="objetivo")
        ax[0].set_ylabel("cmH2O"); ax[0].legend(loc="upper right")
        ax[1].plot(t, [q * 60 for q in d["q_y"]], lw=1)
        ax[1].set_ylabel("flujo (L/min)")
        ax[2].plot(t, [(x - min(v)) * 1000 for x in v], lw=1)
        ax[2].set_ylabel("volumen (mL)"); ax[2].set_xlabel("t (s)")
        fig.tight_layout()
        fig.savefig(a.plot, dpi=110)
        print(f"Gráfica: {a.plot}")


if __name__ == "__main__":
    main()
