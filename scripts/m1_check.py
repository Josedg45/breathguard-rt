#!/usr/bin/env python3
"""Verifica el resumen de M1 recalculando las estadísticas desde los CSV crudos.

Si lo que calculó latency_test en QNX no coincide con lo que dicen sus propias
muestras, termina con error. También imprime una tabla por escenario.
"""
import csv
import math
import sys
from collections import defaultdict
from pathlib import Path

TOLERANCE_US = 0.01
SAMPLES_FIELDS = {
    "period_min_us": lambda p, j: min(p),
    "period_max_us": lambda p, j: max(p),
    "period_avg_us": lambda p, j: sum(p) / len(p),
    "jitter_min_us": lambda p, j: min(j),
    "jitter_max_us": lambda p, j: max(j),
    "jitter_avg_us": lambda p, j: sum(j) / len(j),
    "jitter_p99_us": lambda p, j: nearest_rank(j, 99.0),
    "jitter_p999_us": lambda p, j: nearest_rank(j, 99.9),
}


def nearest_rank(values, pct):
    ordered = sorted(values)
    # round(): 99.9/100*10000 da 9990.000000000002 y ceil() saltaría al rango 9991.
    return ordered[max(1, math.ceil(round(pct / 100 * len(ordered), 6))) - 1]


def verify(directory):
    """Devuelve las filas del resumen; aborta si alguna no cuadra con sus muestras."""
    rows = list(csv.DictReader((directory / "summary.csv").open()))
    errors = 0
    for row in rows:
        with (directory / f"{row['label']}.csv").open() as f:
            samples = list(csv.DictReader(f))
        periods = [float(s["period_us"]) for s in samples]
        jitters = [float(s["jitter_us"]) for s in samples]
        if len(samples) != int(row["samples"]):
            print(f"ERROR {row['label']}: {len(samples)} muestras, resumen {row['samples']}")
            errors += 1
        for field, compute in SAMPLES_FIELDS.items():
            if abs(compute(periods, jitters) - float(row[field])) > TOLERANCE_US:
                print(f"ERROR {row['label']}.{field}: csv={compute(periods, jitters):.3f} "
                      f"resumen={row[field]}")
                errors += 1
        overruns = sum(p > 1.5 * float(row["period_us"]) for p in periods)
        if overruns != int(row["overruns"]):
            print(f"ERROR {row['label']}.overruns: csv={overruns} resumen={row['overruns']}")
            errors += 1
    if errors:
        sys.exit(f"{errors} discrepancias entre el resumen y las muestras")
    print(f"OK: {len(rows)} corridas verificadas contra sus muestras crudas\n")
    return rows


def scenario(label):
    head, _, tail = label.rpartition("_")
    return head if tail.isdigit() else label


def print_tables(directory, rows):
    print(f"{'corrida':<16}{'j_min':>8}{'j_prom':>9}{'j_p99':>10}{'j_p99.9':>10}"
          f"{'j_max':>12}{'per_prom':>10}{'overruns':>9}")
    for r in rows:
        print(f"{r['label']:<16}{float(r['jitter_min_us']):>8.2f}{float(r['jitter_avg_us']):>9.1f}"
              f"{float(r['jitter_p99_us']):>10.1f}{float(r['jitter_p999_us']):>10.1f}"
              f"{float(r['jitter_max_us']):>12.1f}{float(r['period_avg_us']):>10.1f}"
              f"{int(r['overruns']):>9}")

    groups = defaultdict(list)
    for r in rows:
        groups[scenario(r["label"])].append(r)
    print(f"\n{'escenario':<16}{'n':>3}{'j_prom(media)':>15}{'j_p99(media)':>14}"
          f"{'j_max(peor)':>13}{'overruns(media)':>17}")
    for name, group in groups.items():
        print(f"{name:<16}{len(group):>3}"
              f"{sum(float(r['jitter_avg_us']) for r in group) / len(group):>15.1f}"
              f"{sum(float(r['jitter_p99_us']) for r in group) / len(group):>14.1f}"
              f"{max(float(r['jitter_max_us']) for r in group):>13.1f}"
              f"{sum(int(r['overruns']) for r in group) / len(group):>17.1f}")

    noise_path = directory / "noise_summary.csv"
    if noise_path.exists():
        print(f"\n{'ruido VM':<16}{'hueco_max_us':>14}{'robado_%':>10}{'>100us':>8}{'>1ms':>7}{'>5ms':>7}")
        for r in csv.DictReader(noise_path.open()):
            print(f"{r['label']:<16}{float(r['max_gap_us']):>14.1f}{float(r['stolen_pct']):>10.2f}"
                  f"{r['over_100us']:>8}{r['over_1000us']:>7}{r['over_5000us']:>7}")


def main():
    directory = Path(sys.argv[1] if len(sys.argv) > 1 else "results/m1")
    print_tables(directory, verify(directory))


if __name__ == "__main__":
    main()
