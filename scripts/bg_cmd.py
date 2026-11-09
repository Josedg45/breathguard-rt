#!/usr/bin/env python3
"""bg_cmd.py - comandos del operador desde el host: cambia los ajustes del ventilador en marcha.

El comando va por UDP a la HMI de QNX, que lo reenvia a safety_mon; safety_mon lo valida y lo programa para
la proxima respiracion. Imprime la respuesta (ACK o NAK con el motivo).

Uso: bg_cmd.py [--vm 172.31.1.2] [--port 5006] p_insp=25 [peep=8 rr=15 ie=2 vt=450 mode=pcv|vcv]
"""
import argparse
import socket
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vm", default="172.31.1.2")
    ap.add_argument("--port", type=int, default=5006)
    ap.add_argument("--timeout", type=float, default=1.0)
    ap.add_argument("cmd", nargs="+")
    a = ap.parse_args()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(a.timeout)
    s.sendto(" ".join(a.cmd).encode(), (a.vm, a.port))
    try:
        data, _ = s.recvfrom(512)
    except socket.timeout:
        print("sin respuesta (¿HMI caida o relanzandose?)")
        return 2
    ans = data.decode().strip()
    print(ans)
    return 0 if ans.startswith("ACK") else 1


if __name__ == "__main__":
    sys.exit(main())
