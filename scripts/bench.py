#!/usr/bin/env python3
"""bench.py - the interactive test bench, from Ubuntu: deploy, start, swap patients, inject faults, stop.

Two separate paths, on purpose (the same split a real device has):
  - OPERATOR commands (settings, copilot answers) go by UDP to the HMI, which forwards them to safety_mon, which
    validates them. That is send_operator() here, the same protocol as scripts/bg_cmd.py.
  - BENCH actions (patient type, faults, mains, killing processes) are the physical world and the test engineer.
    They go over SSH to scripts/bench_target.sh on the VM, never through the HMI.

Use from a terminal:
  scripts/bench.py deploy                  build and copy binaries, bench_target.sh and the patient states
  scripts/bench.py start [patient_id]      start the bench on the VM (returns at once)
  scripts/bench.py patient ards_moderate   swap the patient
  scripts/bench.py fault -d 1              any bg_fault options (none = clear all faults)
  scripts/bench.py mains 0|1 | kill hmi|ctrl|copilot | status | stop | list
  scripts/bench.py op p_insp=25 fio2=60    an operator command (via the HMI)
The dashboard (scripts/dashboard.py --control) uses the same functions. Environment: BG_VM (address for UDP, default
172.31.1.2), BG_SSH (SSH target, default root@172.31.1.2), BG_HOST (this host as seen from the VM, default
172.31.1.1).
"""
import os
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patients import IDS, LABEL, HINT  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VM = os.environ.get("BG_VM", "172.31.1.2")
SSH_TARGET = os.environ.get("BG_SSH", "root@172.31.1.2")
HOST = os.environ.get("BG_HOST", "172.31.1.1")
DIR = "/tmp/medpower"
SSH = ["ssh", "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=5", "-o", "BatchMode=yes"]
SCP = ["scp", "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=5", "-o", "BatchMode=yes", "-q"]
BINS = ["pneumatic_sim", "breath_ctrl", "safety_mon", "power_mgr", "hmi", "ventd", "copilot", "bg_fault", "patient"]
STATES_DIR = os.path.expanduser("~/pulse/build/install/bin/states")
OP_PORT = 5006
BUSY = "NAK busy:"     # SET_BUSY_PREFIX in src/settings.h, as forwarded by the HMI
RETRY_WAIT_S = 0.3


def ssh(cmd, timeout=30):
    """Run a command on the VM. Returns (ok, text)."""
    try:
        r = subprocess.run(SSH + [SSH_TARGET, cmd], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return False, "ssh: timeout (is the VM up?)"
    out = (r.stdout + r.stderr).strip()
    return r.returncode == 0, out or ("ok" if r.returncode == 0 else f"ssh exit {r.returncode}")


def bench(*args, timeout=30):
    """A bench_target.sh subcommand on the VM."""
    return ssh(f"sh {DIR}/bench_target.sh " + " ".join(str(a) for a in args), timeout)


def send_operator(cmd, retry=3, timeout=1.0):
    """An operator command through the HMI (UDP), like bg_cmd.py. Returns the answer (ACK/NAK ...)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        for attempt in range(retry + 1):
            s.sendto(cmd.encode(), (VM, OP_PORT))
            try:
                data, _ = s.recvfrom(512)
            except socket.timeout:
                return "NAK no answer (HMI down or restarting?)"
            ans = data.decode(errors="replace").strip()
            if not ans.startswith(BUSY) or attempt == retry:
                return ans
            time.sleep(RETRY_WAIT_S)
    finally:
        s.close()
    return ans


def deploy(log=print):
    """Build, copy the binaries and the bench script to /tmp/medpower and the patient states to /data/pulse."""
    r = subprocess.run(["bash", "-c", "source ~/qnx800/qnxsdp-env.sh > /dev/null && make -s all patient"],
                       cwd=REPO, capture_output=True, text=True)
    if r.returncode != 0:
        log(r.stdout + r.stderr)
        return False
    ok, out = ssh(f"mkdir -p {DIR} /data/pulse/states")
    if not ok:
        log(out)
        return False
    for b in BINS:  # temporary name + mv: a running binary cannot be overwritten (CLAUDE.md)
        if subprocess.run(SCP + [os.path.join(REPO, "build", b), f"{SSH_TARGET}:{DIR}/{b}.new"]).returncode:
            log(f"scp {b} failed")
            return False
        ssh(f"mv -f {DIR}/{b}.new {DIR}/{b} && chmod +x {DIR}/{b}")
    subprocess.run(SCP + [os.path.join(REPO, "scripts", "bench_target.sh"), f"{SSH_TARGET}:{DIR}/"], check=True)
    have = set(ssh("ls /data/pulse/states")[1].split())
    for pid in IDS:
        name = f"BG_{pid}.json"
        src = os.path.join(STATES_DIR, name)
        if name in have:
            continue
        if not os.path.exists(src):
            log(f"missing {src}: build it with tools/pulse_states (see its header)")
            continue
        log(f"copying patient {pid}")
        subprocess.run(SCP + [src, f"{SSH_TARGET}:/data/pulse/states/"], check=True)
    log("deploy: ok")
    return True


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 1
    cmd, rest = argv[1], argv[2:]
    if cmd == "list":
        for pid in IDS:
            print(f"{pid:15s} {LABEL[pid]:20s} {HINT[pid]}")
        return 0
    if cmd == "deploy":
        return 0 if deploy() else 1
    if cmd == "op":
        ans = send_operator(" ".join(rest))
        print(ans)
        return 0 if ans.startswith("ACK") else 1
    if cmd == "start":
        ok, out = bench("start", HOST, *(rest or ["healthy_male"]))
    elif cmd in ("patient", "fault", "mains", "kill", "status", "stop"):
        if cmd == "patient" and (not rest or rest[0] not in IDS):
            print(f"unknown patient; one of: {' '.join(IDS)}")
            return 1
        ok, out = bench(cmd, *rest)
    else:
        print(f"unknown command '{cmd}'")
        return 1
    print(out)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
