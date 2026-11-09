#!/usr/bin/env python3
"""boot_watch.py - mide el arranque de la VM QNX desde el host (punto C). Lo llama vm_run.sh.

Desde t0 (QEMU lanzado) registra: primer ping respondido, primer paquete de la HMI (UDP 5005) y puerto 22 abierto.
Luego lee por ssh la hora de la primera inspiración de breath_ctrl (reloj ClockCycles de QNX) y el reloj actual
(/dev/ventilator/status, clock_s) para pasarla a la escala del host.
Uso: boot_watch.py t0_host log_consola
"""
import re
import socket
import subprocess
import sys
import time

VM, PORT = "172.31.1.2", 5005
SSH = ["ssh", "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=2", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new", f"root@{VM}"]


def main():
    t0, log = float(sys.argv[1]), sys.argv[2]
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("0.0.0.0", PORT))
    udp.setblocking(False)
    got = {}
    end = time.time() + 180
    while time.time() < end and len(got) < 3:
        now = time.time()
        if "ping" not in got and subprocess.run(["ping", "-c1", "-W1", VM], capture_output=True).returncode == 0:
            got["ping"] = time.time() - t0
        try:
            udp.recv(4096)
            got.setdefault("hmi", time.time() - t0)
        except BlockingIOError:
            pass
        if "ssh" not in got:
            s = socket.socket()
            s.settimeout(0.3)
            if s.connect_ex((VM, 22)) == 0:
                got["ssh"] = time.time() - t0
            s.close()
        time.sleep(max(0.0, 0.05 - (time.time() - now)))
    time.sleep(2)
    t_q = time.time()
    out = subprocess.run(SSH + ["cat /dev/ventilator/status; cat /data/var/log/bg/safety.txt "
                                "/data/var/log/bg/plant.txt"], capture_output=True, text=True).stdout
    t_r = time.time()
    clock = re.search(r"clock_s=([\d.]+)", out)
    first = re.search(r"primera inspiración t=([\d.]+)", out)
    print(f"boot_watch: desde que se lanzó QEMU (host, s):")
    for k, name in (("ping", "responde al ping"), ("ssh", "sshd escucha (puerto 22)"),
                    ("hmi", "primer paquete de la HMI")):
        print(f"  {name:32s} {got.get(k, float('nan')):7.2f}")
    if clock and first:
        zero = (t_q + t_r) / 2 - float(clock.group(1))  # instante del host en que el reloj de QNX valía 0
        print(f"  {'primera inspiración':32s} {zero + float(first.group(1)) - t0:7.2f}"
              f"   (reloj QNX {float(first.group(1)):.3f} s; error de la conversión ±{(t_r - t_q) / 2:.2f} s)")
        print(f"  {'reloj QNX en 0 (respecto a QEMU)':32s} {zero - t0:7.2f}")
    else:
        print("boot_watch: no encontré la primera inspiración o clock_s en la VM")
    print(out)


if __name__ == "__main__":
    main()
