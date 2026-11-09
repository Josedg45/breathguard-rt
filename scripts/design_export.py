#!/usr/bin/env python3
"""design_export.py - junta los registros crudos de results/design/ en los CSV de design/ (para Simulink) y
compara QNX contra la referencia ideal del host.

Entradas (scripts/design_run.sh): ctrl_dref.csv + sim_dref.csv (PCV en QNX), step_dstep.csv + sim_dstep.csv
(escalones en QNX), ref_pcv_offline.csv y step_offline.csv (host, reloj ideal).
Salidas: design/ref_pcv_qnx.csv.gz, ref_pcv_offline.csv.gz, step_qnx.csv.gz, step_offline.csv.gz, compare.txt.

Las corridas de QNX se registran en dos procesos: el controlador (o step_test) guarda lo que midio y mando en
cada ciclo; la planta, su estado verdadero en cada tick de 1 ms. El estado verdadero se interpola linealmente al
instante de cada ciclo del controlador.
"""
import bisect
import csv
import gzip
import math
import os

RAW, OUT = "results/design", "design"
V0, C_PAT, PEEP = 2.3, 0.058, 5.0
V_REST = V0 + C_PAT * PEEP
COLS = ["t_s", "phase", "setpoint", "paw_meas", "flow_meas", "blower_cmd", "exh_cmd", "peep_cmd", "paw_true",
        "flow_true", "volume_ml", "pbl", "palv"]


def load(name):
    with open(os.path.join(RAW, name)) as f:
        return [{k: float(v) for k, v in r.items()} for r in csv.DictReader(f)]


def interp(plant, t):
    """Estado de la planta en el instante t (interpolacion lineal entre ticks)."""
    ts = plant["t"]
    i = min(max(bisect.bisect_left(ts, t), 1), len(ts) - 1)
    t0, t1 = ts[i - 1], ts[i]
    w = 0.0 if t1 == t0 else min(max((t - t0) / (t1 - t0), 0.0), 1.0)
    return {k: plant[k][i - 1] + w * (plant[k][i] - plant[k][i - 1]) for k in ("paw", "q_y", "v_l", "pbl")}


def plant_series(rows):
    return {"t": [r["t_s"] for r in rows], "paw": [r["paw"] for r in rows], "q_y": [r["q_y"] for r in rows],
            "v_l": [r["v_l"] for r in rows], "pbl": [r["pbl"] for r in rows]}


def merge(ctrl, plant, step):
    p = plant_series(plant)
    t0 = ctrl[0]["t_s"]
    out = []
    for r in ctrl:
        s = interp(p, r["t_s"])
        out.append({
            "t_s": r["t_s"] - t0, "phase": -1 if step else int(r["phase"]),
            "setpoint": 0.0 if step else r["target"], "paw_meas": r["paw_meas"], "flow_meas": r["flow_meas"],
            "blower_cmd": r["blower"], "exh_cmd": r["exh_open"], "peep_cmd": PEEP,
            "paw_true": s["paw"], "flow_true": s["q_y"], "volume_ml": (s["v_l"] - V_REST) * 1e3, "pbl": s["pbl"],
            "palv": (s["v_l"] - V0) / C_PAT,
            "period_ms": r.get("period_ms", 0.0), "exec_us": r.get("exec_us", 0.0)})
    return out


def write(name, rows, cols):
    path = os.path.join(OUT, name)
    with gzip.open(path, "wt", newline="") as f:
        w = csv.writer(f)
        w.writerow(cols)
        for r in rows:
            w.writerow([f"{r[c]:.6g}" if isinstance(r[c], float) else r[c] for c in cols])
    return path


def compare(qnx, ref, label, t_from, t_to):
    """RMS y maximo de la diferencia QNX - ideal, en los mismos instantes (interpolando la ideal)."""
    rt = [r["t_s"] for r in ref]
    lines = []
    for k, unit in (("paw_true", "cmH2O"), ("flow_true", "L/s"), ("volume_ml", "mL")):
        rv = [r[k] for r in ref]
        d = []
        for r in qnx:
            if not t_from <= r["t_s"] <= t_to:
                continue
            i = min(max(bisect.bisect_left(rt, r["t_s"]), 1), len(rt) - 1)
            w = (r["t_s"] - rt[i - 1]) / (rt[i] - rt[i - 1])
            d.append(r[k] - (rv[i - 1] + w * (rv[i] - rv[i - 1])))
        rms = math.sqrt(sum(x * x for x in d) / len(d))
        lines.append(f"  {label:6s} {k:10s} RMS {rms:8.4f} {unit:6s} max |diff| {max(abs(x) for x in d):8.4f}")
    return lines


def main():
    os.makedirs(OUT, exist_ok=True)
    pcv = merge(load("ctrl_dref.csv"), load("sim_dref.csv"), False)
    stp = merge(load("step_dstep.csv"), load("sim_dstep.csv"), True)
    ref = load("ref_pcv_offline.csv")
    ref_s = load("step_offline.csv")
    extra = COLS + ["period_ms", "exec_us"]
    print("design_export:", write("ref_pcv_qnx.csv.gz", pcv, extra))
    print("design_export:", write("step_qnx.csv.gz", stp, extra))
    for r in ref + ref_s:
        r["phase"] = int(r["phase"])
    print("design_export:", write("ref_pcv_offline.csv.gz", ref, COLS))
    print("design_export:", write("step_offline.csv.gz", ref_s, COLS))

    per = sorted(r["period_ms"] for r in pcv[1:])
    late = sum(1 for x in per if x > 1.5)
    rep = ["QNX run vs ideal 1 ms reference (same equations, same sensor-noise generator)", "",
           f"PCV 20/5 QNX: {len(pcv)} cycles over {pcv[-1]['t_s']:.1f} s; period p50 {per[len(per) // 2]:.3f} ms, "
           f"p99 {per[int(len(per) * 0.99)]:.3f} ms, max {per[-1]:.2f} ms, {late} cycles > 1.5 ms",
           "Difference over the whole 60 s (QNX - ideal):"]
    rep += compare(pcv, ref, "PCV", 0.0, 59.0)
    rep += ["", "Open-loop steps (blower 5 -> 20 cmH2O at 1 s, exhalation valve opens at 10 s):"]
    rep += compare(stp, ref_s, "steps", 0.0, 15.5)
    rep += ["", "The large max |diff| values occur only at the inspiration -> expiration switch (t = 1.667 s into each",
            "breath), where Paw falls ~15 cmH2O within a few ms: a clock offset of one cycle gives that difference.",
            "", "Differences come from timing: the QNX loop runs on the measured clock (late cycles, VM noise), the",
            "ideal reference on an exact 1 ms grid. The sensor noise is generated with the same seed, but the number",
            "of reads differs (safety_mon and the HMI do not run here; the plant also samples once per READ)."]
    with open(os.path.join(OUT, "compare.txt"), "w") as f:
        f.write("\n".join(rep) + "\n")
    print("\n".join(rep))


if __name__ == "__main__":
    main()
