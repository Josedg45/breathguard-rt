#!/usr/bin/env python3
"""M2: ¿un modelo pulmonar RC a 1 kHz reproduce lo que hace Pulse a su paso lento?

Lee CSV de `pulse_probe external`. Ajusta R, C y V0 con la primera parte del primer archivo y evalúa
con el resto y con los demás archivos, que usan otros ajustes: el pulmón no es lineal y una prueba
con la misma respiración no lo revelaría. Evalúa dos formas de usar el modelo:

  libre:   integra sin corrección durante toda la corrida.
  acople:  cada 20 ms se resincroniza con el estado de Pulse usando los R y C que él reporta, e
           integra solo hasta el paso siguiente. Es lo que hará pneumatic_sim; el error es lo que el
           modelo rápido se desvía de Pulse dentro de esos 20 ms.

Modelo:  flujo = (Paw - Palv) / R ;  Palv = (V - V0) / C ;  dV/dt = flujo
"""
import csv
import sys

import numpy as np

DT_FAST_S = 0.001
SETTLE_S = 30.0       # se descarta el transitorio al conectar el ventilador
TRAIN_FRACTION = 0.5


def load(path):
    with open(path) as f:
        rows = list(csv.DictReader(f))
    return {key: np.array([float(r[key]) for r in rows]) for key in rows[0]}


def fit_rc(paw, volume, dt):
    """Regresión lineal de dV/dt = a*Paw + b*V + c. Devuelve R, C y V0."""
    dv_dt = np.diff(volume) / dt
    design = np.column_stack([paw[:-1], volume[:-1], np.ones(len(dv_dt))])
    (a, b, c), *_ = np.linalg.lstsq(design, dv_dt, rcond=None)
    resistance = 1.0 / a
    compliance = -a / b
    v0 = c / -b
    return resistance, compliance, v0


def simulate(paw, v_start, resistance, compliance, v0, dt_slow):
    """Integra con Euler a 1 kHz manteniendo Paw constante dentro de cada paso lento."""
    substeps = round(dt_slow / DT_FAST_S)
    out = np.empty(len(paw))
    volume = v_start
    for i, pressure in enumerate(paw):
        for _ in range(substeps):
            palv = (volume - v0) / compliance
            volume += DT_FAST_S * (pressure - palv) / resistance
        out[i] = volume
    return out


def coupled_step_error(paw, volume, flow, resistance, compliance, dt_slow):
    """Error de volumen al llegar al siguiente paso, resincronizando en cada uno."""
    substeps = round(dt_slow / DT_FAST_S)
    predicted = np.empty(len(paw) - 1)
    for k in range(len(paw) - 1):
        # El estado de Pulse fija la presión alveolar; R y C son los que él reporta en ese paso.
        palv = paw[k] - flow[k] * resistance[k]
        v = volume[k]
        v0 = v - compliance[k] * palv
        for _ in range(substeps):
            palv_now = (v - v0) / compliance[k]
            v += DT_FAST_S * (paw[k + 1] - palv_now) / resistance[k]
        predicted[k] = v
    return predicted - volume[1:]


def report(name, diff, tidal):
    rms = float(np.sqrt(np.mean(diff ** 2)))
    worst = float(np.max(np.abs(diff)))
    print(f"  {name:<9} RMS={rms:7.2f} mL ({100 * rms / tidal:5.2f} % del Vt)   "
          f"max={worst:7.2f} mL ({100 * worst / tidal:5.2f} %)")


def evaluate(label, data, r_fit, c_fit, v0_fit, dt):
    paw, volume = data["paw_set_cmH2O"], data["lung_volume_mL"]
    tidal = float(np.median(data["tidal_volume_mL"]))
    print(f"{label}: Vt={tidal:.0f} mL, Paw {paw.min():.0f}-{paw.max():.0f} cmH2O, "
          f"{np.median(data['rate_per_min']):.0f} rpm; Pulse reporta "
          f"R={np.median(data['resistance_cmH2O_s_L']):.1f} C={np.median(data['compliance_mL_cmH2O']):.1f}")
    report("libre", simulate(paw, volume[0], r_fit, c_fit, v0_fit, dt) - volume, tidal)
    # Pulse reporta R en cmH2O·s/L y el modelo trabaja en mL.
    report("acople", coupled_step_error(paw, volume, data["insp_flow_mL_s"],
                                        data["resistance_cmH2O_s_L"] / 1e3,
                                        data["compliance_mL_cmH2O"], dt), tidal)


def main():
    if len(sys.argv) < 2:
        sys.exit("uso: m2_rc_check.py <csv de ajuste> [otros csv con otros ajustes...]")
    d = load(sys.argv[1])
    dt = float(np.median(np.diff(d["t_s"])))
    keep = d["t_s"] >= SETTLE_S
    trimmed = {key: values[keep] for key, values in d.items()}
    split = int(len(trimmed["t_s"]) * TRAIN_FRACTION)
    train = {key: values[:split] for key, values in trimmed.items()}
    test = {key: values[split:] for key, values in trimmed.items()}

    r_fit, c_fit, v0_fit = fit_rc(train["paw_set_cmH2O"], train["lung_volume_mL"], dt)
    print(f"dt de Pulse = {dt * 1e3:.1f} ms; ajuste con {sys.argv[1]}, "
          f"{train['t_s'][0]:.0f}-{train['t_s'][-1]:.0f} s")
    print(f"R={r_fit * 1e3:.2f} cmH2O·s/L  C={c_fit:.1f} mL/cmH2O  V0={v0_fit:.0f} mL  "
          f"tau={r_fit * c_fit:.3f} s\n")

    evaluate("Misma respiración, tiempo no visto", test, r_fit, c_fit, v0_fit, dt)
    for path in sys.argv[2:]:
        other = load(path)
        trimmed_other = {key: values[other["t_s"] >= SETTLE_S] for key, values in other.items()}
        evaluate(path.rsplit("/", 1)[-1], trimmed_other, r_fit, c_fit, v0_fit, dt)


if __name__ == "__main__":
    main()
