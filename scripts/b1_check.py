#!/usr/bin/env python3
"""b1_check.py - evalúa la prueba de la tarea 4 (scripts/b1_run.sh): una HMI comprometida (evil_hmi) intenta
mover el soplador. Pasa si:
  1. evil_hmi informa que la planta rechazó todo (AUTH, OVERRIDE y cada WRITE);
  2. la presión en la vía aérea durante el ataque no supera la de las respiraciones previas (+1.5 cmH2O);
  3. safety_mon registró el evento COMANDO_NO_AUTORIZADO;
  4. la HMI siguió mandando datos sin conectarse a la planta (lee de safety_mon).
Uso: b1_check.py carpeta etiqueta
"""
import csv
import os
import re
import sys

WIN_S = 4.0  # ataque de 2 s + margen: una respiración completa a 12 rpm dura 5 s


def main():
    d, lab = sys.argv[1], sys.argv[2]
    log = open(os.path.join(d, f"log_{lab}.txt"), encoding="utf-8", errors="replace").read()
    ok = True

    def res(name, good, detail):
        nonlocal ok
        ok &= good
        print(f"{'OK   ' if good else 'FALLA'} {name}: {detail}")

    m = re.search(r"evil_hmi: pid \d+, t_abs=([\d.]+)", log)
    if not m:
        print("FALLA: no hay salida de evil_hmi en el log")
        return 1
    t_att = float(m.group(1))
    lines = [x for x in log.splitlines() if x.startswith("evil_hmi: ") and re.match(r"evil_hmi: \d\.", x)]
    blocked = "ataque bloqueado" in log
    res("1. la planta rechaza el ataque", blocked, "; ".join(x[10:] for x in lines))

    t, paw = [], []
    with open(os.path.join(d, f"sim_{lab}.csv")) as f:
        for r in csv.DictReader(f):
            t.append(float(r["t_s"]))
            paw.append(float(r["paw"]))
    att = [p for ti, p in zip(t, paw) if t_att <= ti < t_att + WIN_S]
    base = [p for ti, p in zip(t, paw) if t_att - 3 * WIN_S <= ti < t_att]
    pa, pb = max(att), max(base)
    res("2. presión durante el ataque", pa <= pb + 1.5,
        f"máx {pa:.1f} cmH2O en los {WIN_S:.0f} s del ataque; máx {pb:.1f} en los {3 * WIN_S:.0f} s anteriores")

    with open(os.path.join(d, f"ev_{lab}.csv")) as f:
        evs = [r for r in csv.DictReader(f) if r["evento"] == "COMANDO_NO_AUTORIZADO"]
    det = f"t = {float(evs[0]['t_s']) - t_att:+.3f} s desde el ataque: {evs[0]['detalle']}" if evs else "no aparece"
    res("3. safety_mon registra el evento", bool(evs), det)

    with open(os.path.join(d, f"hmi_{lab}.csv")) as f:
        rows = [r for r in csv.DictReader(f) if r["t"] != "RT"]  # t_host, luego la línea de la HMI
    paws = [float(r["paw"]) for r in rows]
    res("4. la HMI muestra datos (vía safety_mon)", len(rows) > 1000 and max(paws) > 15,
        f"{len(rows)} muestras, Paw máx {max(paws) if paws else 0:.1f} cmH2O")

    m = re.search(r"comandos rechazados por autorización (\d+)", log)
    print(f"      planta: {m.group(1) if m else '?'} comandos rechazados")
    print("\nPRUEBA B1:", "PASA" if ok else "FALLA")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
