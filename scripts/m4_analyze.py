#!/usr/bin/env python3
"""m4_analyze.py - analiza una escena de seguridad de M4 (results/m4/{sim,pat,ev,log}_<etiqueta>).

- Eventos de safety_mon con su tiempo relativo.
- Falla del controlador: hueco sin mando (último comando de breath_ctrl -> primer comando de safety_mon),
  duración del respaldo y devolución del mando.
- Fallas inyectadas: latencia de la alarma desde la inyección (bg_fault, reloj común).
- Vt por respiración y SpO2/EtCO2 de Pulse alrededor de la falla.
Uso: m4_analyze.py etiqueta [--plot]
"""
import csv
import gzip
import os
import re
import sys


def load(path):
    if not os.path.exists(path) and os.path.exists(path + ".gz"):
        path += ".gz"
    with (gzip.open(path, "rt") if path.endswith(".gz") else open(path)) as f:
        rows = list(csv.DictReader(f))
    return rows


def main():
    lab = sys.argv[1]
    plot = "--plot" in sys.argv
    sim = load(f"results/m4/sim_{lab}.csv")
    ev = load(f"results/m4/ev_{lab}.csv")
    pat = load(f"results/m4/pat_{lab}.csv")
    log = open(f"results/m4/log_{lab}.txt").read()

    t = [float(r["t_s"]) for r in sim]
    src = [int(r["src"]) for r in sim]
    age = [float(r["cmd_age_ms"]) for r in sim]
    t0 = float(ev[0]["t_s"])  # LANZA_CONTROL

    print(f"Eventos de safety_mon (t relativo al arranque de safety_mon):")
    for e in ev:
        print(f"  {float(e['t_s']) - t0:8.3f} s  {e['evento']:24s} {e['detalle']}")

    # Falla del controlador: primer tick en que manda safety_mon por respaldo.
    fail = [e for e in ev if e["evento"] in ("CONTROL_MUERTO", "CONTROL_COLGADO", "APNEA_CON_CONTROL_VIVO")]
    if fail:
        tf = float(fail[0]["t_s"])
        i = next(k for k in range(len(t)) if t[k] >= tf - 0.5 and src[k] == 1)
        # Antes del cambio, cmd_age dice hace cuánto llegó el último comando de breath_ctrl.
        last_ctrl = t[i - 1] - age[i - 1] / 1e3
        print(f"\nFalla del controlador ({fail[0]['evento']}):")
        print(f"  último comando de breath_ctrl -> safety_mon detecta: {(tf - last_ctrl) * 1e3:7.1f} ms")
        print(f"  último comando de breath_ctrl -> planta obedece al respaldo: {(t[i] - last_ctrl) * 1e3:7.1f} ms "
              f"(resolución: tick de 1 ms de la planta)")
        hb = [e for e in ev if e["evento"] == "DEVUELVE_MANDO" and float(e["t_s"]) > tf]
        if hb:
            print(f"  respaldo activo: {float(hb[0]['t_s']) - tf:6.2f} s; mando devuelto al nuevo controlador")

    # Fallas inyectadas: líneas "falla t_abs=..." de pneumatic_sim.
    faults = [(float(m.group(1)), m.group(0)) for m in re.finditer(r"falla t_abs=([\d.]+)[^\n]*", log)]
    for tfa, txt in faults:
        alarms = [e for e in ev if float(e["t_s"]) >= tfa and e["evento"] not in ("RELANZA_CONTROL",)]
        first = alarms[0] if alarms else None
        desc = txt.split(" ", 2)[-1]
        if first and float(first["t_s"]) - tfa < 20:
            print(f"\nFalla inyectada en {tfa - t0:.3f} s ({desc}):\n  primera alarma {first['evento']} a "
                  f"{(float(first['t_s']) - tfa) * 1e3:.1f} ms")
        else:
            print(f"\nFalla inyectada en {tfa - t0:.3f} s ({desc}): sin alarma en 20 s")

    # Vt por respiración (ciclo de 5 s desde t0 + 0.5) y signos vitales.
    v = [float(r["v_l"]) for r in sim]
    tb0, period, ti = t0 + 0.5, 5.0, 5.0 / 3
    print("\nVt por respiración (mL), quién mandaba, y SpO2 de Pulse al final de cada una:")
    tp = [float(r["t_wall"]) for r in pat]
    k = 0
    line = []
    i = 0
    tp_abs0 = None
    # t_wall de patient es relativo a su arranque: lo alineamos con t_plant (tiempo de planta desde su arranque).
    tplant = [float(r["t_plant"]) for r in pat]
    sim_start = t[0] - float(sim[0]["dt_ms"]) / 1e3
    while tb0 + (k + 1) * period <= t[-1]:
        st, en = tb0 + k * period, tb0 + k * period + ti
        while i < len(t) and t[i] < st:
            i += 1
        j, vmax, s_any = i, v[i], 0
        while j < len(t) and t[j] < st + period:
            if t[j] < en:
                vmax = max(vmax, v[j])
            s_any |= src[j]
            j += 1
        # SpO2 al final de la respiración: ventana de Pulse cuyo t_plant corresponde.
        tpl = (st + period) - sim_start
        q = min(range(len(tplant)), key=lambda x: abs(tplant[x] - tpl))
        line.append(f"{(vmax - v[i]) * 1000:4.0f}{'*' if s_any else ' '} ({float(pat[q]['spo2']) * 100:.1f}%)")
        k += 1
    for n in range(0, len(line), 6):
        print("  " + "  ".join(line[n:n + 6]))
    print("  (* = safety_mon tuvo el mando en algún momento de esa respiración)")
    spo2 = [float(r["spo2"]) for r in pat[50:]]
    et = [float(r["etco2"]) for r in pat[50:]]
    print(f"SpO2 mín/máx: {min(spo2) * 100:.2f} / {max(spo2) * 100:.2f} %   EtCO2 mín/máx: {min(et):.1f} / {max(et):.1f}")

    if plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        tr = [x - t0 for x in t]
        fig, ax = plt.subplots(3, 1, sharex=True, figsize=(11, 7))
        ax[0].plot(tr, [float(r["paw"]) for r in sim], lw=0.8)
        ax[0].set_ylabel("Paw (cmH2O)")
        ax[1].plot(tr, [float(r["q_y"]) * 60 for r in sim], lw=0.8)
        ax[1].set_ylabel("flujo (L/min)")
        ax[2].plot([x - (t0 - sim_start) for x in tplant], [float(r["spo2"]) * 100 for r in pat], lw=1)
        ax[2].set_ylabel("SpO2 (%)")
        ax[2].set_xlabel("t desde el arranque de safety_mon (s)")
        for a in ax:
            for e in ev:
                if e["evento"] not in ("LANZA_CONTROL", "CONTROL_TERMINA"):
                    a.axvline(float(e["t_s"]) - t0, color="red", lw=0.6, alpha=0.6)
            ys = [tr[n] for n in range(len(src)) if src[n]]
            if ys:
                a.axvspan(min(ys), max(ys), color="orange", alpha=0.15)
        ax[0].set_title(f"{lab}: líneas rojas = eventos de safety_mon; naranja = manda safety_mon")
        fig.tight_layout()
        fig.savefig(f"results/m4/{lab}.png", dpi=110)
        print(f"Gráfica: results/m4/{lab}.png")


if __name__ == "__main__":
    main()
