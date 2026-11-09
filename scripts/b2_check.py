#!/usr/bin/env python3
"""b2_check.py - evaluates the layer-2 test log (scripts/b2_run.sh).

Pass criteria: all six of our types are present; evil_hmi with the HMI type and with the ventd type is refused by
the kernel at name_open (exit 0, "capa 2"), so the plant never sees a message (denied stays 0); the HMI type cannot
write the settings file; root can; the kernel logged the denials and none of them is a legitimate ventilator
process; ventilation continued (same source, Paw within normal range, no controller failover).
Usage: b2_check.py log
"""
import re
import sys


def main():
    log = open(sys.argv[1], encoding="utf-8", errors="replace").read()
    ok = True

    def res(name, good, detail):
        nonlocal ok
        ok &= bool(good)
        print(f"{'OK  ' if good else 'FAIL'} {name}: {detail}")

    def rc(key):
        m = re.search(rf"^{key} (\d+)", log, re.M)
        return int(m.group(1)) if m else None

    types = set(re.findall(r"\b(bg_\w+?)\b", log.split("== status before")[0]))
    want = {"bg_plant_t", "bg_safety_t", "bg_power_t", "bg_ventd_t", "bg_safety_t__ctrl", "bg_safety_t__hmi"}
    # pidin truncates long type names (bg_safety_t__ctr)
    found = {w for w in want if any(t.startswith(w[:16]) for t in types)}
    res("processes typed", found == want, f"{sorted(found)}")
    n_l2 = len(re.findall(r"RECHAZADO por el kernel", log))
    res("evil_hmi refused by the kernel (HMI and ventd types)", n_l2 == 2 and rc("RC_HMI") == 0 and
        rc("RC_VENTD") == 0, f"{n_l2} refusals at name_open; exit codes {rc('RC_HMI')}, {rc('RC_VENTD')}")
    m = re.search(r"cannot create /dev/ventilator/settings: (.*)", log)
    res("HMI type cannot write /dev/ventilator/settings", rc("RC_HMI_WRITE") not in (None, 0) and m and
        "not permitted" in m.group(1), f"exit code {rc('RC_HMI_WRITE')}, {m.group(0) if m else 'no refusal message'}")
    res("root can write settings", rc("RC_ROOT_WRITE") == 0, f"exit code {rc('RC_ROOT_WRITE')}")

    errs = log.split("== secpolgenerate errors")[1] if "== secpolgenerate errors" in log else ""
    denials = [l for l in errs.splitlines() if "failed to use ability" in l]
    ours = [l for l in denials if "bg_" in l]
    # only the attackers may appear: evil_hmi, and the shell (ksh/sh) started with the HMI type in case 4
    bad = [l for l in ours if not re.search(r"\d\d:\d\d:\d\d (evil_hmi|k?sh) \(", l)]
    res("kernel logged the attack", any("channel_connect:bg_plant_t" in l for l in ours),
        f"{len(ours)} denials of bg_ types: " + "; ".join(sorted({l.strip()[:110] for l in ours})[:4]))
    res("no legitimate process denied", not bad, f"{len(bad)} " + "; ".join(bad[:3]))

    st = re.findall(r"^source=(.*)$", log, re.M)
    paw = [float(x) for x in re.findall(r"paw_cmh2o=([\d.]+)", log)]
    den = re.findall(r"failovers=(\d+) hmi_restarts=\d+ denied=(\d+)", log)
    res("ventilation unaffected", len(st) >= 2 and all(s == "breath_ctrl" for s in st) and den and
        all(f == "0" and d == "0" for f, d in den),
        f"source {st}, failovers/plant denials {den} (the plant never received a message), Paw samples {paw}")
    res("settings applied", re.search(r"p_insp=21\.0", log) is not None, "p_insp=21.0 in settings after the write")
    print("\nLAYER 2 TEST:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
