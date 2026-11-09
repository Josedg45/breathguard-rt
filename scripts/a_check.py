#!/usr/bin/env python3
"""a_check.py - evalúa la prueba de /dev/ventilator (scripts/a_run.sh).

Funcionalidad: lecturas, escritura aceptada y aplicada, rechazos con código de error, archivo de solo lectura, y
que un lector se bloquee (sin afectar a nadie más) mientras ventd está congelado.
Aislamiento: la antigüedad del comando en la planta (cada 1 ms) en tres ventanas: línea base, un cliente leyendo
/dev/ventilator/status sin parar, y ventd congelado. Si el controlador nunca deja de mandar, el p99 se queda cerca
de 1 ms. El máximo se informa, pero no se exige: incluye las pausas de la VM.
Uso: a_check.py carpeta etiqueta
"""
import csv
import math
import os
import re
import sys


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, math.ceil(round(p / 100 * len(v), 6)) - 1)]


def main():
    d, lab = sys.argv[1], sys.argv[2]
    log = open(os.path.join(d, f"log_{lab}.txt"), encoding="utf-8", errors="replace").read()
    ok = True

    def res(name, good, detail):
        nonlocal ok
        ok &= bool(good)
        print(f"{'OK   ' if good else 'FALLA'} {name}: {detail}")

    def num(key):
        m = re.search(rf"^{key} (\d+)", log, re.M)
        return int(m.group(1)) if m else None

    t0 = num("T0")
    marks = {k: int(v) - t0 for k, v in re.findall(r"^MARCA (\w+) (\d+)", log, re.M)}
    n = num("LECTURAS")
    res("lecturas de status", n and n > 50, f"{n} lecturas completas (cat) en 10 s")
    res("contenido de status", "source=breath_ctrl" in log and "paw_cmh2o=" in log, "tiene fuente, ajustes y Paw")
    m = re.search(r"^CAT_TERMINO (\d+)", log, re.M)
    t_cat = int(m.group(1)) - t0 if m else None
    res("lector bloqueado con ventd congelado", t_cat is not None and t_cat >= marks.get("descongelado", 1e9),
        f"el cat terminó a t={t_cat} s; ventd se congeló a {marks.get('congelado')} y se soltó a {marks.get('descongelado')}")
    rc = {k: num(k) for k in ("RC_OK", "RC_RANGO", "RC_MAL", "RC_RO")}
    res("escritura válida", rc["RC_OK"] == 0, f"código {rc['RC_OK']}")
    res("rechazos con error", all(rc[k] not in (None, 0) for k in ("RC_RANGO", "RC_MAL", "RC_RO")),
        f"fuera de rango {rc['RC_RANGO']}, mal formada {rc['RC_MAL']}, status de solo lectura {rc['RC_RO']}")
    res("ajuste aplicado", "AJUSTE_APLICADO" in log and re.search(r"^mode=pcv p_insp=22\.0", log, re.M),
        "safety_mon lo aplicó y settings muestra p_insp=22.0")
    m = re.search(r"safety_mon: \d+ ticks, (\d+) fallas", log)
    res("sin fallas del controlador", m and m.group(1) == "0", f"{m.group(1) if m else '?'} fallas")

    t, age = [], []
    with open(os.path.join(d, f"sim_{lab}.csv")) as f:
        for r in csv.DictReader(f):
            t.append(float(r["t_s"]))
            age.append(float(r["cmd_age_ms"]))
    tp = t[0] + 1.0  # la planta arranca ~1 s antes de T0 (sleep 1 en a_target.sh)
    wins = [("línea base", 2, 10), ("cliente leyendo sin parar", marks["lectura_inicio"] + 0.5,
                                    marks["lectura_fin"] - 0.5),
            ("ventd congelado", marks["congelado"] + 0.5, marks["descongelado"] - 0.5)]
    print("\nAntigüedad del comando en la planta (ms), por ventana:")
    print(f"  {'ventana':28s} {'ticks':>6s} {'p50':>6s} {'p99':>6s} {'p99.9':>6s} {'máx':>7s}")
    for name, a, b in wins:
        v = [x for ti, x in zip(t, age) if a <= ti - tp < b]
        print(f"  {name:28s} {len(v):6d} {pct(v, 50):6.2f} {pct(v, 99):6.2f} {pct(v, 99.9):6.2f} {max(v):7.2f}")
        ok &= pct(v, 99) < 2.0
    print("  (criterio: p99 < 2 ms en las tres; el máximo incluye las pausas de la VM)")
    print("\nPRUEBA /dev/ventilator:", "PASA" if ok else "FALLA")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
