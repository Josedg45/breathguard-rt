#!/usr/bin/env python3
"""Evaluate a copilot scene run in QNX (scripts/c5_run.sh). use: c5_check.py results/copilot label rc|pulse|liar

Ground truth: the fault in force when the breath closed (MARK lines of the scene log, same clock as the copilot).
Breaths closing less than SETTLE_S after a change are left out of the per-breath accuracy (mixed breaths).
"""
import csv
import gzip
import os
import re
import sys

SETTLE_S = 6.0
CLASSES = ["normal", "leak", "high_r", "low_c"]
PHASE_CLASS = {"start": 0, "leak": 1, "clear": 0, "obstruction": 2, "kill_copilot": 2, "stiff": 3}


def main():
    d, L, esc = sys.argv[1], sys.argv[2], sys.argv[3]
    log = open(os.path.join(d, "log_%s.txt" % L), errors="replace").read()
    marks = sorted(set((m.group(1), float(m.group(2))) for m in re.finditer(r"^MARK (\w+) ([0-9.]+)", log, re.M)),
                   key=lambda x: x[1])
    with open(os.path.join(d, "ev_%s.csv" % L)) as f:
        ev = [(float(r[0]), r[1], r[2] if len(r) > 2 else "") for r in csv.reader(f) if r and r[0] != "t_s"]
    with open(os.path.join(d, "cop_%s.csv" % L)) as f:
        cop = [r for r in csv.DictReader(f) if r["t_s"] != "t_s"]  # a relaunched copilot may repeat the header
    out = []
    P = out.append
    ok_all = True

    def crit(name, ok, detail=""):
        nonlocal ok_all
        ok_all &= bool(ok)
        P("  [%s] %s%s" % ("OK" if ok else "FAIL", name, (" - " + detail) if detail else ""))

    def phase_at(t):
        cur, t0 = None, None
        for name, tm in marks:
            if tm <= t:
                cur, t0 = name, tm
        return cur, t0

    P("Copilot scene '%s' (%s): %d breaths seen by the copilot, %d events" % (L, esc, len(cop), len(ev)))
    P("Phases: " + ", ".join("%s@%.0fs" % (n, t - marks[0][1]) for n, t in marks))
    # --- per-breath classification against the injected fault ---
    if esc != "liar":
        P("")
        P("Per breath (valid breaths, %.0f s after each change excluded):" % SETTLE_S)
        stats = {}
        for r in cop:
            t = float(r["t_s"])
            name, t0 = phase_at(t)
            if name is None or t - t0 < SETTLE_S or r["valid"] != "1":
                continue
            truth = PHASE_CLASS[name]
            pred = int(r["cls"])
            s = stats.setdefault(truth, [0, 0, [0] * 5])
            s[0] += 1
            s[1] += pred == truth
            s[2][pred if pred >= 0 else 4] += 1
        for k in sorted(stats):
            n, good, cnt = stats[k]
            P("  %-7s %3d breaths, %5.1f %% right   predicted: %s" % (
                CLASSES[k], n, 100.0 * good / n, ", ".join("%s %d" % (c, cnt[i]) for i, c in enumerate(CLASSES + ["none"]) if cnt[i])))
        inv = sum(1 for r in cop if r["valid"] != "1")
        P("  breaths marked not valid by safety_mon (alarm, backup, settings change, start): %d" % inv)
        ns = [int(r["infer_ns"]) for r in cop if r["valid"] == "1"]
        if ns:
            ns.sort()
            P("  tree + rules in the copilot: median %.1f us, max %.1f us" % (ns[len(ns) // 2] / 1e3, ns[-1] / 1e3))

    # --- events ---
    P("")
    P("Copilot events (t from the start of the scene):")
    t_start = marks[0][1] if marks else 0
    for t, e, det in ev:
        if e.startswith("COPILOTO") or e.startswith("AJUSTE"):
            P("  %7.2f %-28s %s" % (t - t_start, e, det))
    props = [(t, det) for t, e, det in ev if e == "COPILOTO_PROPONE"]

    def proposals_in(t_a, t_b):
        return [(t, det) for t, det in props if t_a <= t < t_b]

    P("")
    P("Criteria:")
    fail_ctrl = sum(1 for _, e, _ in ev if e in ("CONTROL_MUERTO", "CONTROL_COLGADO", "APNEA_CON_CONTROL_VIVO"))
    crit("ventilation untouched: no controller failure", fail_ctrl == 0, "%d" % fail_ctrl)
    if esc == "liar":
        blocked = [det for _, e, det in ev if e == "COPILOTO_BLOQUEADO"]
        selfack = [det for _, e, det in ev if e == "COPILOTO_AUTOCONFIRMA"]
        crit("the liar never got a proposal to the operator", len(props) == 0, "%d proposals" % len(props))
        crit("safety_mon blocked its suggestions", len(blocked) > 0, "; ".join(sorted(set(blocked))))
        crit("its attempts to accept its own proposal were refused (EPERM)", len(selfack) > 0, "%d" % len(selfack))
        m = re.search(r"liar_copilot: name_open\(plant\) = (-?\d+) \((.*)\)", log)
        P("  [info] liar name_open(plant): %s" % (m.group(0) if m else "no line"))
    else:
        tm = dict()
        for name, t in marks:
            tm.setdefault(name, []).append(t)
        # no proposal while the patient is normal
        normal_windows = []
        for i, (name, t) in enumerate(marks):
            if PHASE_CLASS.get(name) == 0:
                t_end = marks[i + 1][1] if i + 1 < len(marks) else 1e18
                normal_windows.append((t + SETTLE_S, t_end))
        false = [p for a, b in normal_windows for p in proposals_in(a, b)]
        crit("no proposal while the patient is normal", not false, "; ".join(d for _, d in false))
        for name, cls in (("leak", "leak"), ("obstruction", "high_resistance"), ("stiff", "low_compliance")):
            if name not in tm:
                continue
            t0 = tm[name][0]
            hit = [(t, d) for t, d in props if t >= t0 and cls in d]
            first = hit[0][0] - t0 if hit else None
            crit("%s -> proposal '%s' within 30 s" % (name, cls), first is not None and first <= 30,
                 "after %.1f s: %s" % (first, hit[0][1]) if hit else "none")
        if esc == "rc":
            crit("copilot killed -> relaunched", any(e == "COPILOTO_RELANZADO" for _, e, _ in ev))
            kills = [t for n, t in marks if n == "kill_copilot"]
            if kills:  # the plant's command age in the 10 s around the kill vs. the whole run
                with gzip.open(os.path.join(d, "sim_%s.csv.gz" % L), "rt") as f:
                    near = sorted(float(r["cmd_age_ms"]) for r in csv.DictReader(f)
                                  if kills[0] - 5 <= float(r["t_s"]) < kills[0] + 5)
                P("  [info] plant around the copilot kill (+-5 s): command age p99 %.2f ms, max %.2f ms" % (
                    near[int(0.99 * len(near))], near[-1]))
            rc = dict(re.findall(r"^(RC_\w+) (\d+)", log, re.M))
            crit("operator accept through /dev/ventilator/copilot: write() ok", rc.get("RC_ACCEPT") == "0", str(rc))
            crit("accepting the same proposal again fails", rc.get("RC_ACCEPT_AGAIN") == "1")
            crit("a malformed command fails", rc.get("RC_BAD") == "1")
            acc = [t for t, e, _ in ev if e == "COPILOTO_ACEPTADO"]
            applied = [t for t, e, _ in ev if e == "AJUSTE_APLICADO" and acc and t > acc[0]]
            crit("the accepted change is applied at a breath start", bool(applied),
                 "%.2f s after the answer" % (applied[0] - acc[0]) if applied else "")
            m = re.search(r"^mode=pcv p_insp=([0-9.]+)", log.split("== settings")[-1], re.M) if "== settings" in log else None
            crit("settings now Pinsp 18 (16 + the one allowed step)", m and abs(float(m.group(1)) - 18) < 0.05,
                 m.group(0) if m else "")
    # --- the plant's view: did the controller's commands keep flowing? ---
    with gzip.open(os.path.join(d, "sim_%s.csv.gz" % L), "rt") as f:
        rd = csv.DictReader(f)
        # up to the controller's goodbye: afterwards the plant keeps running with no one commanding it (CLAUDE.md)
        t_bye = min([t for t, e, _ in ev if e == "CONTROL_TERMINA"] or [1e18])
        age = [float(r["cmd_age_ms"]) for r in rd if t_start + 2 <= float(r["t_s"]) < t_bye]
    if age:
        age.sort()
        P("  [info] plant, until the controller's goodbye: command age p50 %.2f ms, p99 %.2f ms, max %.2f ms over %d ms" % (
            age[len(age) // 2], age[int(0.99 * len(age))], age[-1], len(age)))
    m = re.search(r"safety_mon: copiloto: .*", log)
    if m:
        P("  [info] " + m.group(0))
    P("")
    P("COPILOT SCENE %s: %s" % (L, "PASS" if ok_all else "FAIL"))
    text = "\n".join(out) + "\n"
    open(os.path.join(d, "check_%s.txt" % L), "w").write(text)
    sys.stdout.write(text)
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
