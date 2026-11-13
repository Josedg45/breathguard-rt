#!/usr/bin/env python3
"""c5hmi_run.py - the operator answers the copilot from the host, through the HMI (point 5).

RC plant, PCV 16/5, with the HMI sending telemetry to this host. At 30 s the lung becomes stiff (bg_fault -c 2.5).
The script reads the copilot's "CP," lines like the dashboard does, waits for the proposal and answers it the way
an operator would, with scripts/bg_cmd.py:
  copilot accept <id>       -> ACK, and the settings move by the one allowed step at a breath start
  copilot accept <id> again -> NAK (no longer pending)
  copilot approve <id>      -> NAK (malformed)
It records everything (results/copilot/hmi_<label>.csv, replayable with dashboard.py --replay) and checks it.

Use: scripts/c5hmi_run.py label [copilot_binary]   (default build/copilot: with the task 5 skeleton the tree never
     answers, so there is no proposal and the scene FAILS on purpose)
"""
import os
import socket
import subprocess
import sys
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VM, HOST, DIR = "root@172.31.1.2", "172.31.1.1", "/tmp/medpower"
SSH = ["ssh", "-o", "LogLevel=ERROR"]
SCP = ["scp", "-o", "LogLevel=ERROR", "-q"]
SECS = 90            # length of the scene on the VM
STIFF_AT = 30        # s: the lung becomes stiff
P_INSP = 16.0


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, text=True, capture_output=True, **kw).stdout


def put(src, name):
    """Copy under another name first: a running binary cannot be overwritten (CLAUDE.md)."""
    sh(SCP + [src, f"{VM}:{DIR}/{name}.new"])
    sh(SSH + [VM, f"mv -f {DIR}/{name}.new {DIR}/{name} && chmod +x {DIR}/{name}"])


def bg_cmd(*words):
    r = subprocess.run([sys.executable, os.path.join(REPO, "scripts/bg_cmd.py"), "--retry", "0", *words],
                       text=True, capture_output=True)
    return r.stdout.strip()


def main():
    label = sys.argv[1]
    cop = sys.argv[2] if len(sys.argv) > 2 else os.path.join(REPO, "build/copilot")
    os.chdir(REPO)
    bins = ["pneumatic_sim", "breath_ctrl", "safety_mon", "ventd", "bg_fault", "hmi"]
    subprocess.run(["make", "-s"] + [f"build/{b}" for b in bins], check=True)
    sh(SSH + [VM, f"mkdir -p {DIR}; [ -x /system/bg/bg_stop.sh ] && /system/bg/bg_stop.sh; true"])
    for b in bins:
        put(f"build/{b}", b)
    put(cop, "copilot")
    out = os.path.join(REPO, "results/copilot")
    os.makedirs(out, exist_ok=True)

    # Host side: record what the HMI sends (same format as hmi_recv.py, so dashboard.py --replay reads it).
    rows, stop = [], threading.Event()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", 5005))
    sock.settimeout(0.3)

    def recv():
        while not stop.is_set():
            try:
                data, _ = sock.recvfrom(2048)
            except socket.timeout:
                continue
            rows.append((time.time(), data.decode(errors="replace").strip()))
    th = threading.Thread(target=recv, daemon=True)
    th.start()

    target = (f"cd {DIR}; rm -f ev_{label}.csv; "
              f"./pneumatic_sim -s {SECS + 4} -e 5 > log_sim_{label}.txt 2>&1 & sleep 1; "
              f"./safety_mon -s {SECS} -m pcv -P {P_INSP:.0f} -e 5 -K -k ./copilot -H {HOST} -o ev_{label}.csv "
              f"> log_safety_{label}.txt 2>&1 & "
              f"./ventd -s {SECS - 1} > log_ventd_{label}.txt 2>&1 & "
              f"sleep {STIFF_AT}; ./bg_fault -c 2.5 > /dev/null; wait")
    vm = subprocess.Popen(SSH + [VM, target], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    t0 = time.time()
    log = []

    def note(msg):
        line = f"{time.time() - t0:7.2f} {msg}"
        log.append(line)
        print(line)

    # Wait for a pending proposal in the CP lines, as the operator would see it on the dashboard.
    pid_ = None
    while time.time() - t0 < SECS - 10 and pid_ is None:
        time.sleep(0.5)
        for _, line in reversed(rows):
            if line.startswith("CP,"):
                cp = dict(kv.partition("=")[::2] for kv in line[3:].split(","))
                if cp.get("state") == "pending":
                    pid_ = int(cp["id"])
                    note(f"dashboard shows proposal #{pid_}: {cp.get('pcls')} - {cp.get('text')}")
                break
    res = {}
    if pid_ is not None:
        time.sleep(2)  # the operator reads it
        res["accept"] = bg_cmd("copilot", "accept", str(pid_))
        note(f"operator: copilot accept {pid_} -> {res['accept']}")
        res["again"] = bg_cmd("copilot", "accept", str(pid_))
        note(f"operator: copilot accept {pid_} again -> {res['again']}")
        res["bad"] = bg_cmd("copilot", "approve", str(pid_))
        note(f"operator: copilot approve {pid_} -> {res['bad']}")
    vm_out, _ = vm.communicate(timeout=SECS + 60)
    time.sleep(1)
    stop.set()
    th.join()
    t_ans = t0 + next((float(x.split()[0]) for x in log if "accept" in x and "again" not in x), 1e9)

    csv = os.path.join(out, f"hmi_{label}.csv")
    with open(csv, "w") as f:
        f.write("t_host,t,paw,flow,spo2,etco2,hr,vit_age,src,alarms,backup,failovers,hmi_restarts,last_event,"
                "pwr_bits,soc,autonomy_min,hmi_pid,r,c_ml,vcv,p_insp,peep,rr,ie,vt_ml,set_seq,set_pending,"
                "cmd_age_ms\n")
        for t, line in rows:
            f.write(f"{t:.4f},{line}\n")
    sh(SCP + [f"{VM}:{DIR}/ev_{label}.csv", out + "/"])
    sh(SSH + [VM, f"rm -f {DIR}/ev_{label}.csv {DIR}/log_*_{label}.txt /data/var/dumper/*.core; sync"])
    with open(os.path.join(out, f"log_{label}.txt"), "w") as f:
        f.write("\n".join(log) + "\n" + vm_out)

    # Settings as the HMI reported them, before and after the answer.
    tele = [(t, line.split(",")) for t, line in rows if line[:1].isdigit()]
    before = [float(p[20]) for t, p in tele if t < t_ans and len(p) >= 28]
    after = [(t, float(p[20])) for t, p in tele if t > t_ans and len(p) >= 28]
    applied = next((t for t, v in after if v == P_INSP + 2), None)
    cps = [line for _, line in rows if line.startswith("CP,")]
    ev = open(os.path.join(out, f"ev_{label}.csv")).read()

    crit = [
        ("the HMI sent copilot lines (CP,) at about 1 Hz", len(cps) >= SECS * 0.7, f"{len(cps)} lines"),
        ("the dashboard saw a pending proposal after the lung became stiff", pid_ is not None, f"#{pid_}"),
        ("operator accept through the HMI -> ACK", res.get("accept", "").startswith("ACK"), res.get("accept")),
        ("accepting it again -> NAK", res.get("again", "").startswith("NAK"), res.get("again")),
        ("malformed answer -> NAK", res.get("bad", "").startswith("NAK"), res.get("bad")),
        ("Pinsp was 16 before the answer", bool(before) and before[-1] == P_INSP, before[-1] if before else None),
        (f"Pinsp {P_INSP + 2:.0f} after it (one step), applied at a breath start", applied is not None,
         f"{applied - t_ans:.2f} s after the answer" if applied else None),
        ("safety_mon logged the operator's accept", "COPILOTO_ACEPTADO" in ev, ""),
        ("ventilation untouched: no controller failure", "CONTROL_MUERTO" not in ev and "CONTROL_COLGADO" not in ev,
         ""),
    ]
    print("\nCriteria:")
    ok = True
    for name, good, info in crit:
        ok &= bool(good)
        print(f"  [{'OK' if good else 'FAIL'}] {name} - {info}")
    print(f"\nCOPILOT HMI SCENE {label}: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
