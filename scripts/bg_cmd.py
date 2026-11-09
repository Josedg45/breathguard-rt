#!/usr/bin/env python3
"""bg_cmd.py - comandos del operador desde el host: cambia los ajustes del ventilador en marcha.

El comando va por UDP a la HMI de QNX, que lo reenvia a safety_mon; safety_mon lo valida y lo programa para
la proxima respiracion. Imprime la respuesta (ACK o NAK con el motivo).

If a previous change starts in less than 0.2 s, safety_mon answers "NAK busy: ..." (src/settings.h,
settings_schedule). That refusal is transient, so the command is resent after the pending change has started
(--retry times, default 3; the intermediate refusals go to stderr, the final answer to stdout).

Uso: bg_cmd.py [--vm 172.31.1.2] [--port 5006] [--retry 3] p_insp=25 [peep=8 rr=15 ie=2 vt=450 mode=pcv|vcv]
"""
import argparse
import socket
import sys
import time

BUSY = "NAK busy:"   # SET_BUSY_PREFIX in src/settings.h, as forwarded by the HMI
RETRY_WAIT_S = 0.3   # > SET_LEAD_S (0.2 s): by then the pending change has started


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vm", default="172.31.1.2")
    ap.add_argument("--port", type=int, default=5006)
    ap.add_argument("--timeout", type=float, default=1.0)
    ap.add_argument("--retry", type=int, default=3,
                    help="resend up to N times while a previous change is about to start (NAK busy); 0 = never")
    ap.add_argument("cmd", nargs="+")
    a = ap.parse_args()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(a.timeout)
    cmd = " ".join(a.cmd).encode()
    for attempt in range(max(a.retry, 0) + 1):
        s.sendto(cmd, (a.vm, a.port))
        try:
            data, _ = s.recvfrom(512)
        except socket.timeout:
            print("sin respuesta (¿HMI caida o relanzandose?)")
            return 2
        ans = data.decode().strip()
        if not ans.startswith(BUSY) or attempt == a.retry:
            break
        print(f"{ans} (resending in {RETRY_WAIT_S} s)", file=sys.stderr)
        time.sleep(RETRY_WAIT_S)
    print(ans)
    return 0 if ans.startswith("ACK") else 1


if __name__ == "__main__":
    sys.exit(main())
