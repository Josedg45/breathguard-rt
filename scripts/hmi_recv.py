#!/usr/bin/env python3
"""hmi_recv.py - recibe la telemetría UDP de la HMI (QNX) y la guarda en CSV. Sin interfaz gráfica.
Uso: hmi_recv.py salida.csv segundos [puerto]
"""
import socket
import sys
import time

COLS = ("t,paw,flow,spo2,etco2,hr,vit_age,src,alarms,backup,failovers,hmi_restarts,last_event,"
        "pwr_bits,soc,autonomy_min,hmi_pid,r,c_ml,vcv,p_insp,peep,rr,ie,vt_ml,set_seq,set_pending")


def main():
    out, secs = sys.argv[1], float(sys.argv[2])
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 5005
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", port))
    s.settimeout(0.5)
    n = 0
    end = time.time() + secs
    with open(out, "w") as f:
        f.write("t_host," + COLS + "\n")
        while time.time() < end:
            try:
                data, _ = s.recvfrom(2048)
            except socket.timeout:
                continue
            f.write(f"{time.time():.4f},{data.decode().strip()}\n")
            n += 1
    print(f"hmi_recv: {n} muestras en {out}")


if __name__ == "__main__":
    main()
