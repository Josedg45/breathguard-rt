#!/usr/bin/env python3
"""dashboard.py - M6: monitor del ventilador en el host (Ubuntu). Dibuja en vivo la telemetria UDP de la HMI.

Curvas de presion, flujo y volumen de los ultimos 15 s, signos vitales de Pulse, quien controla
(breath_ctrl o el respaldo de safety_mon), alarmas, energia y estado de la propia HMI. Si la HMI muere o se
cuelga, el dashboard lo muestra ("NO DATA") mientras el paciente sigue ventilado en QNX.

Uso:
  dashboard.py                              en vivo, escucha UDP en el puerto 5005
  dashboard.py --replay hmi_x.csv[.gz]      reproduce una corrida grabada por hmi_recv.py
  dashboard.py --replay f.csv --snapshot out.png --at 40   guarda una imagen del instante 40 s (sin ventana)
  dashboard.py --record demo.csv            en vivo y ademas graba lo recibido (para --replay)
Subtitulos para el video: una linea UDP "CAPTION,texto" al mismo puerto (la manda scripts/demo_run.sh).
La interfaz va en ingles (la ve el jurado); los comentarios, en espanol como el resto del codigo.
"""
import argparse
import collections
import gzip
import math
import socket
import sys
import time

import matplotlib

COLS = ("t,paw,flow,spo2,etco2,hr,vit_age,src,alarms,backup,failovers,hmi_restarts,last_event,"
        "pwr_bits,soc,autonomy_min,hmi_pid,r,c_ml,vcv,p_insp,peep,rr,ie,vt_ml,set_seq,set_pending,cmd_age_ms").split(",")
N_OK = (19, 27, len(COLS))  # formatos anteriores (monolito, M6 antes de los ajustes y de cmd_age) y el actual
ALARMS = ["HIGH PRESSURE", "DISCONNECTION", "APNEA", "CONTROLLER DEAD", "CONTROLLER HUNG", "LOW Vt", "HIGH Vt",
          "LEAK", "POWER"]
WINDOW_S = 15.0      # segundos de curva visibles
# Codigos de evento de safety_mon (en espanol en el log) -> texto para la pantalla.
EVENTS = {"ALIVIO_PRESION": "pressure relief", "APNEA_CON_CONTROL_VIVO": "apnea, controller alive",
          "BATERIA_BAJA": "battery low", "BATERIA_CRITICA": "battery critical",
          "CONTROL_COLGADO": "controller hung", "CONTROL_MUERTO": "controller died",
          "CONTROL_TERMINA": "controller exited", "DEVUELVE_MANDO": "control handed back",
          "ENERGIA_CORTE": "mains lost", "ENERGIA_RED": "mains restored", "HMI_COLGADA": "HMI hung, killed",
          "HMI_MUERTA_RELANZADA": "HMI died, relaunched", "LANZA_CONTROL": "controller started",
          "LANZA_HMI": "HMI started", "RELANZA_CONTROL": "controller relaunched",
          "AJUSTE_ACEPTADO": "settings accepted", "AJUSTE_RECHAZADO": "settings REJECTED",
          "AJUSTE_APLICADO": "settings applied"}
GAP_S = 0.3          # sin datos por mas de esto = la HMI no esta enviando
# Colores: superficie oscura de monitor; una serie por panel; colores de estado reservados para estados.
BG, PANEL, INK, MUTED, GRID = "#1a1a19", "#222220", "#ecebe7", "#9a9993", "#3a3a37"
C_PAW, C_FLOW, C_VOL = "#3987e5", "#199e70", "#d95926"
GOOD, WARN, SERIOUS, CRIT = "#0ca30c", "#fab219", "#ec835a", "#d03b3b"


def parse(line):
    """Una linea CSV de la HMI -> dict (o {"caption": texto}). Devuelve None si no se puede leer."""
    if line.startswith("CAPTION,"):
        return {"caption": line[8:].strip()}
    p = line.strip().split(",")
    if len(p) not in N_OK:
        return None
    d = dict(zip(COLS, p))
    try:
        for k in d:
            if k != "last_event":
                d[k] = float(d[k])
    except ValueError:
        return None
    return d


class State:
    """Buffer de muestras y valores derivados (volumen integrado, Vt, picos por respiracion)."""

    def __init__(self):
        self.t = collections.deque()
        self.paw = collections.deque()
        self.flow = collections.deque()
        self.vol = collections.deque()
        self.last = None
        self.last_rx = None          # reloj del host al recibir la ultima muestra
        self.v = 0.0
        self.neg_time = 0.0          # tiempo con flujo <= 0 (espiracion)
        self.breath = {"vmax": 0.0, "pmax": 0.0}
        self.vt = self.ppeak = self.peep = None
        # PEEP = presion al final de la espiracion: muestras de 50-100 ms antes de detectar la inspiracion (al
        # detectarla, la presion ya esta subiendo).
        self.recent_paw = collections.deque(maxlen=10)
        self.hmi_pids = []
        self.caption = ("", 0.0)
        self.ages = collections.deque(maxlen=100)  # (t, antigüedad máx. del comando en ms) del último segundo

    def add(self, d, rx):
        if "caption" in d:
            self.caption = (d["caption"], rx)
            return
        if self.last is not None:
            dt = d["t"] - self.last["t"]
            if dt > 0.1 or dt < 0:          # hueco: cortar la curva
                for q in (self.t, self.paw, self.flow, self.vol):
                    q.append(math.nan)
                dt = 0.0
            # Inicio de respiracion: flujo inspiratorio claro (> 9 L/min) tras >= 0.5 s sin inspirar. Umbrales
            # con margen: al final de la espiracion el flujo ronda 0 con ruido y cruza el cero muchas veces.
            if d["flow"] > 0.15 and self.neg_time >= 0.5:
                b = self.breath
                self.vt, self.ppeak = b["vmax"] * 1e3, b["pmax"]
                old = list(self.recent_paw)[:5]
                self.peep = sum(old) / len(old)
                self.v = 0.0
                self.breath = {"vmax": 0.0, "pmax": 0.0}
            # Histeresis: el contador solo se reinicia al superar el mismo umbral que dispara. Si se reiniciara
            # antes (entre 0.10 y 0.15 L/s), una subida muestreada justo ahi nunca dispararia.
            if d["flow"] > 0.15:
                self.neg_time = 0.0
            elif d["flow"] < 0.1:
                self.neg_time += dt
            self.v += d["flow"] * dt
        b = self.breath
        b["vmax"] = max(b["vmax"], self.v)
        b["pmax"] = max(b["pmax"], d["paw"])
        self.recent_paw.append(d["paw"])
        self.t.append(d["t"])
        self.paw.append(d["paw"])
        self.flow.append(d["flow"] * 60.0)  # L/s -> L/min
        self.vol.append(self.v * 1e3)       # L -> mL
        while self.t and (math.isnan(self.t[0]) or self.t[0] < d["t"] - WINDOW_S - 1):
            for q in (self.t, self.paw, self.flow, self.vol):
                q.popleft()
        if "cmd_age_ms" in d:
            self.ages.append((d["t"], d["cmd_age_ms"]))
        pid = int(d["hmi_pid"])
        if not self.hmi_pids or self.hmi_pids[-1] != pid:
            self.hmi_pids.append(pid)
        self.last = d
        self.last_rx = rx


def status_lines(st, now_rx):
    """Lineas del panel derecho: (texto, color, tamano)."""
    d = st.last
    out = []
    if d is None:
        return [("Waiting for telemetry...", MUTED, 13)]
    gap = now_rx - st.last_rx
    if gap > GAP_S:
        out.append((f"NO DATA FROM HMI  {gap:4.1f} s", CRIT, 14))
        out.append(("values below are frozen", MUTED, 9))
        out.append(("(display process down; QNX keeps ventilating)" if d["last_event"] != "MONO"
                    else "(single process: nobody is watching)", MUTED, 9))
    else:
        out.append((f"HMI pid {int(d['hmi_pid'])}  receiving", GOOD, 11))
    out.append(("", INK, 6))
    stale = d["vit_age"] > 1.0 or d["spo2"] < 0
    spo2 = "--" if d["spo2"] < 0 else f"{d['spo2'] * 100:.1f}"
    col_spo2 = CRIT if d["spo2"] >= 0 and d["spo2"] < 0.90 else WARN if d["spo2"] >= 0 and d["spo2"] < 0.94 else INK
    out.append((f"SpO2   {spo2} %", col_spo2, 20))
    out.append((f"EtCO2  {d['etco2']:.1f} mmHg", INK, 13))
    out.append((f"HR     {d['hr']:.0f} bpm" + ("   (vitals stale)" if stale else ""), INK, 13))
    out.append(("", INK, 6))
    vt = "--" if st.vt is None else f"{st.vt:.0f} mL"
    pk = "--" if st.ppeak is None else f"{st.ppeak:.1f} / {st.peep:.1f}"
    out.append((f"Vt     {vt}", INK, 13))
    out.append((f"Ppeak / PEEP  {pk} cmH2O", INK, 11))
    out.append((f"Patient R {d['r']:.1f} cmH2O·s/L   C {d['c_ml']:.0f} mL/cmH2O", MUTED, 9))
    if "p_insp" in d:
        mode = "VCV" if d["vcv"] else "PCV"
        main = f"Vt {d['vt_ml']:.0f} mL" if d["vcv"] else f"Pinsp {d['p_insp']:.0f}"
        tag = " (next)" if d["set_pending"] else ""
        out.append((f"Set {mode} {main} / PEEP {d['peep']:.0f} / {d['rr']:.0f} bpm{tag}",
                    WARN if d["set_pending"] else INK, 9))
    if st.ages:
        recent = [a for t, a in st.ages if t > d["t"] - 1.0]
        age = max(recent) if recent else st.ages[-1][1]
        col = GOOD if age < 5 else WARN if age < 50 else CRIT
        out.append((f"Loop 1 kHz: command age max {age:5.1f} ms", col, 10))
    out.append(("", INK, 6))
    if d["last_event"] == "MONO":
        out.append(("Controller: MONOLITHIC", SERIOUS, 11))
    elif d["backup"]:
        out.append(("Controller: BACKUP (safety_mon)", SERIOUS, 11))
    else:
        out.append(("Controller: breath_ctrl", GOOD, 11))
    al = int(d["alarms"])
    names = [n for i, n in enumerate(ALARMS) if al >> i & 1]
    if names:
        for n in names:
            out.append((f"! ALARM  {n}", CRIT, 12))
    else:
        out.append(("Alarms: none", GOOD, 11))
    if d["last_event"] not in ("-", "MONO"):
        out.append((f"Last event: {EVENTS.get(d['last_event'], d['last_event'])}", MUTED, 9))
    if d["last_event"] != "MONO":
        out.append((f"Controller failovers {int(d['failovers'])}   HMI restarts {int(d['hmi_restarts'])}", MUTED, 9))
        out.append(("", INK, 6))
        bits = int(d["pwr_bits"])
        if d["soc"] <= 0 and bits == 0:
            out.append(("Power: no data", MUTED, 11))
        else:
            src = "BATTERY" if bits & 1 else "MAINS"
            col = CRIT if bits & 4 else WARN if bits & 2 else (SERIOUS if bits & 1 else GOOD)
            out.append((f"Power: {src}   SoC {d['soc'] * 100:.0f} %", col, 11))
            extra = "   LOAD SHEDDING" if bits & 8 else ""
            if bits & 1:
                out.append((f"Autonomy {d['autonomy_min']:.0f} min{extra}", col, 10))
    return out


def build_figure(plt):
    fig = plt.figure(figsize=(13, 7.2), facecolor=BG)
    fig.canvas.manager.set_window_title("BreathGuard RT") if hasattr(fig.canvas, "manager") and fig.canvas.manager \
        else None
    gs = fig.add_gridspec(3, 2, width_ratios=[2.6, 1], hspace=0.28, wspace=0.08,
                          left=0.06, right=0.98, top=0.87, bottom=0.07)
    axes = []
    specs = [("Airway pressure (cmH2O)", C_PAW, (-2, 42)), ("Flow (L/min)", C_FLOW, (-90, 90)),
             ("Volume (mL)", C_VOL, (-50, 800))]
    lines = []
    for i, (title, col, ylim) in enumerate(specs):
        ax = fig.add_subplot(gs[i, 0], facecolor=PANEL)
        ax.set_xlim(-WINDOW_S, 0)
        ax.set_ylim(*ylim)
        ax.set_title(title, color=INK, fontsize=10, loc="left", pad=3)
        ax.tick_params(colors=MUTED, labelsize=8)
        for s in ax.spines.values():
            s.set_color(GRID)
        ax.grid(True, color=GRID, linewidth=0.6)
        if i < 2:
            ax.set_xticklabels([])
        (ln,) = ax.plot([], [], color=col, linewidth=2)
        lines.append(ln)
        axes.append(ax)
    axes[-1].set_xlabel("seconds", color=MUTED, fontsize=9)
    axes[1].axhline(0, color=MUTED, linewidth=0.8)
    side = fig.add_subplot(gs[:, 1], facecolor=PANEL)
    side.set_xticks([])
    side.set_yticks([])
    for s in side.spines.values():
        s.set_color(GRID)
    fig.text(0.06, 0.955, "BreathGuard RT  ·  QNX 8.0  ·  virtual patient: Pulse", color=INK, fontsize=13,
             weight="bold")
    clock = fig.text(0.98, 0.955, "", color=MUTED, fontsize=10, ha="right")
    caption = fig.text(0.06, 0.905, "", color=WARN, fontsize=13)
    return fig, axes, lines, side, (clock, caption)


CAPTION_S = 12.0  # segundos que se ve un subtitulo


def draw(st, now_rx, lines, side, texts):
    clock, caption = texts
    text, t_cap = st.caption
    caption.set_text(text if text and now_rx - t_cap < CAPTION_S else "")
    if st.t:
        t_end = st.last["t"]
        xs = [x - t_end for x in st.t]
        for ln, ys in zip(lines, (st.paw, st.flow, st.vol)):
            ln.set_data(xs, list(ys))
        clock.set_text(f"QNX clock {t_end:9.2f} s")
    for txt in list(side.texts):
        txt.remove()
    y = 0.97
    for text, col, size in status_lines(st, now_rx):
        side.text(0.05, y, text, color=col, fontsize=size, va="top", family="monospace",
                  transform=side.transAxes)
        y -= 0.012 + size * 0.0042


def read_csv(path):
    op = gzip.open if path.endswith(".gz") else open
    rows = []
    with op(path, "rt") as f:
        next(f)
        for raw in f:
            host, _, rest = raw.partition(",")
            d = parse(rest)
            if d and (rows or "caption" not in d):  # el tiempo de inicio lo da la telemetria
                rows.append((float(host), d))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=5005)
    ap.add_argument("--replay")
    ap.add_argument("--speed", type=float, default=1.0)
    ap.add_argument("--snapshot")
    ap.add_argument("--at", type=float, default=30.0)
    ap.add_argument("--record")
    a = ap.parse_args()

    if a.snapshot:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    st = State()

    if a.snapshot:  # imagen fija de un instante de una corrida grabada
        rows = read_csv(a.replay)
        t0 = rows[0][0]
        for rx, d in rows:
            if rx - t0 > a.at:
                break
            st.add(d, rx)
        fig, axes, lines, side, clock = build_figure(plt)
        draw(st, t0 + a.at, lines, side, clock)
        fig.savefig(a.snapshot, dpi=110, facecolor=BG)
        print(f"dashboard: {a.snapshot} (t = {a.at:.1f} s)")
        return

    fig, axes, lines, side, clock = build_figure(plt)
    if a.replay:
        rows = read_csv(a.replay)
        t0_data, t0_wall, idx = rows[0][0], time.time(), [0]

        def poll():
            now = t0_data + (time.time() - t0_wall) * a.speed
            while idx[0] < len(rows) and rows[idx[0]][0] <= now:
                st.add(rows[idx[0]][1], rows[idx[0]][0])
                idx[0] += 1
            return now
    else:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("0.0.0.0", a.port))
        sock.setblocking(False)
        print(f"dashboard: escuchando UDP {a.port}", file=sys.stderr)
        rec = open(a.record, "w") if a.record else None
        if rec:
            rec.write("t_host," + ",".join(COLS) + "\n")

        def poll():
            while True:
                try:
                    data, _ = sock.recvfrom(2048)
                except BlockingIOError:
                    break
                now, line = time.time(), data.decode(errors="replace").strip()
                if rec:
                    rec.write(f"{now:.4f},{line}\n")
                d = parse(line)
                if d:
                    st.add(d, now)
            if rec:
                rec.flush()
            return time.time()

    from matplotlib.animation import FuncAnimation

    def update(_):
        draw(st, poll(), lines, side, clock)
        return []

    anim = FuncAnimation(fig, update, interval=100, cache_frame_data=False)  # noqa: F841 (debe vivir)
    plt.show()


if __name__ == "__main__":
    main()
