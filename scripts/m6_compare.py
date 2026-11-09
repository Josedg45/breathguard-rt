#!/usr/bin/env python3
"""m6_compare.py - contraste de la demo: el mismo bug de pantalla en BreathGuard (procesos QNX separados) y en
la version monolitica (mono_vent). Lee results/m6/{sim,pat}_<escena>.csv[.gz] y escribe la figura y un resumen.

Uso: m6_compare.py [--out results/m6/contrast.png]
"""
import argparse
import csv
import gzip
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

D = "results/m6"
SCENES = [  # (escena, etiqueta, color categorico fijo)
    ("hmi_bug_90", "BreathGuard: display crashes (NULL pointer)", "#2a78d6"),
    ("hmi_colgar_90", "BreathGuard: display hangs (infinite loop)", "#1baf7a"),
    ("mono_bug", "Monolithic: display crashes (NULL pointer)", "#eb6834"),
    ("mono_colgar", "Monolithic: display hangs (infinite loop)", "#e34948"),
]
T_BUG = 31.0  # s desde el arranque de la planta: la escena inyecta el bug 30 s despues de lanzar el control


def rows(name):
    p = os.path.join(D, name)
    op = gzip.open if os.path.exists(p + ".gz") else open
    with op(p + ".gz" if os.path.exists(p + ".gz") else p, "rt") as f:
        return list(csv.DictReader(f))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(D, "contrast.png"))
    a = ap.parse_args()

    fig, axes = plt.subplots(3, 1, figsize=(10, 8.5), sharex=True,
                             gridspec_kw={"height_ratios": [1.4, 1, 1], "hspace": 0.25})
    ax_s, ax_d, ax_m = axes
    summary = []
    for scene, label, col in SCENES:
        pat = rows(f"pat_{scene}.csv")
        t = [float(r["t_plant"]) for r in pat]
        spo2 = [float(r["spo2"]) * 100 for r in pat]
        ax_s.plot(t, spo2, color=col, linewidth=2, label=label)
        sim = rows(f"sim_{scene}.csv")
        t0 = float(sim[0]["t_s"])
        ts = [float(r["t_s"]) - t0 for r in sim[::5]]
        paw = [float(r["paw"]) for r in sim[::5]]
        if scene == "hmi_bug_90":
            ax_d.plot(ts, paw, color=col, linewidth=1.2)
        if scene == "mono_bug":
            ax_m.plot(ts, paw, color=col, linewidth=1.2)
        age = [float(r["cmd_age_ms"]) for r in sim]
        after = [x for r, x in zip(sim, age) if T_BUG < float(r["t_s"]) - t0 < T_BUG + 25]
        hr = [float(r["hr"]) for r in pat]
        summary.append(f"{scene:12s} SpO2 {spo2[0]:.1f} -> min {min(spo2):.1f} %  HR max {max(hr):.0f}  "
                       f"edad máx del último comando en los 25 s tras el bug {max(after) / 1e3:.2f} s")
    ax_s.set_ylabel("SpO2 (%)")
    ax_s.set_ylim(85, 100)
    ax_s.legend(loc="lower left", fontsize=8, frameon=False)
    ax_s.set_title("Same display bug, two architectures (QNX 8.0 VM, Pulse virtual patient)", loc="left",
                   fontsize=11)
    ax_d.set_ylabel("Paw (cmH2O)")
    ax_d.set_title("BreathGuard RT: control and display in separate QNX processes", loc="left", fontsize=9)
    ax_m.set_ylabel("Paw (cmH2O)")
    ax_m.set_title("Monolithic: control and display in one loop (no supervisor)", loc="left", fontsize=9)
    ax_m.set_xlabel("time (s)")
    for ax in axes:
        ax.axvline(T_BUG, color="#6b6a65", linewidth=1, linestyle="--")
        ax.grid(True, color="#e4e3df", linewidth=0.6)
        for s in ("top", "right"):
            ax.spines[s].set_visible(False)
    ax_s.text(T_BUG + 0.5, 99.2, "display bug injected", fontsize=8, color="#6b6a65")
    ax_s.text(60, 98.4, "BreathGuard, both bugs: SpO2 unchanged (lines overlap)", fontsize=8, color="#3a3a37")
    ax_s.text(60, 91.0, "Monolithic: patient no longer ventilated", fontsize=8, color="#3a3a37")
    for ax in (ax_d, ax_m):
        ax.set_ylim(-1, 25)
    fig.savefig(a.out, dpi=110, bbox_inches="tight")
    print("\n".join(summary))
    print(f"m6_compare: {a.out}")


if __name__ == "__main__":
    main()
