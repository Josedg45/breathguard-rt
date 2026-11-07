#!/usr/bin/env python3
"""M2: compara corridas de Pulse en QNX contra las del host, columna a columna.

uso: m2_compare.py <host.csv> <qnx.csv> [<host.csv> <qnx.csv> ...]
"""
import csv
import sys

import numpy as np

COLUMNS = ["lung_volume_mL", "insp_flow_mL_s", "tidal_volume_mL", "spo2", "etco2_mmHg", "hr_per_min"]


def load(path):
    with open(path) as f:
        rows = list(csv.DictReader(f))
    return {key: np.array([float(r[key]) for r in rows]) for key in rows[0]}


def compare(host_path, qnx_path):
    host, qnx = load(host_path), load(qnx_path)
    if len(host["t_s"]) != len(qnx["t_s"]):
        sys.exit(f"{host_path} y {qnx_path} tienen distinto número de pasos")
    print(f"{qnx_path.rsplit('/', 1)[-1]}  ({len(host['t_s'])} pasos)")
    print(f"  {'columna':<18}{'host final':>12}{'qnx final':>12}{'dif. max':>12}{'dif. RMS':>12}{'iguales':>9}")
    for column in COLUMNS:
        a, b = host[column], qnx[column]
        diff = b - a
        print(f"  {column:<18}{a[-1]:>12.4f}{b[-1]:>12.4f}{np.max(np.abs(diff)):>12.4f}"
              f"{np.sqrt(np.mean(diff ** 2)):>12.4f}{100 * np.mean(diff == 0):>8.1f}%")


def main():
    paths = sys.argv[1:]
    if not paths or len(paths) % 2:
        sys.exit(__doc__)
    for i in range(0, len(paths), 2):
        compare(paths[i], paths[i + 1])


if __name__ == "__main__":
    main()
