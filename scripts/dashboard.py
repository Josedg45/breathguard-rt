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
Metricas de tiempo real (1 Hz): lineas "RT,clave=valor,..." de la HMI (periodo del lazo, plazos incumplidos,
histograma, antigüedad del comando, limites de alarma, auto-PEEP, peso ideal, FiO2).
Marcadores: lineas verticales en las tres curvas cuando cambia algo (falla del controlador, HMI relanzada,
desconexion, corte de red, ajustes aplicados).
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
ALARMS = ["HIGH PRESSURE", "DISCONNECTION", "APNEA", "CONTROLLER DEAD", "CONTROLLER HUNG", "LOW Vt",
          "HIGH Vt (> 8 mL/kg)", "LEAK", "POWER", "AUTO-PEEP"]
# Clases del histograma del periodo (como bg_hist_edge en bg_proto.h): 4 por octava, centradas en 1 ms.
HIST_EDGES = [0.25 * 2 ** ((i - 0.5) / 4) for i in range(29)]
# Umbrales de color de las metricas de tiempo real (ms). Verde: dentro del plazo de 1 ms (+0.5). Ambar: hasta
# 20 ms, una ventana de Pulse; las mediciones de M4-M7 muestran que la ventilacion no cambia. Rojo: mas.
RT_OK_MS, RT_WARN_MS = 1.5, 20.0
VT_KG_LO, VT_KG_HI = 6.0, 8.0  # ventilacion protectora: mL/kg de peso ideal
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
          "AJUSTE_APLICADO": "settings applied", "PRESION_ALTA": "high pressure alarm",
          "DESCONEXION": "disconnection alarm", "APNEA": "apnea alarm", "VT_BAJO": "low Vt alarm",
          "VT_ALTO": "high Vt alarm", "FUGA": "leak alarm", "AUTO_PEEP": "auto-PEEP alarm"}
GAP_S = 0.3          # sin datos por mas de esto = la HMI no esta enviando
# Colores: superficie oscura de monitor; una serie por panel; colores de estado reservados para estados.
BG, PANEL, INK, MUTED, GRID = "#1a1a19", "#222220", "#ecebe7", "#9a9993", "#3a3a37"
C_PAW, C_FLOW, C_VOL = "#3987e5", "#199e70", "#d95926"
GOOD, WARN, SERIOUS, CRIT = "#0ca30c", "#fab219", "#ec835a", "#d03b3b"


def parse(line):
    """Una linea CSV de la HMI -> dict (o {"caption": texto}). Devuelve None si no se puede leer."""
    if line.startswith("CAPTION,"):
        return {"caption": line[8:].strip()}
    if line.startswith("RT,"):
        rt = {}
        for kv in line.strip()[3:].split(","):
            k, _, v = kv.partition("=")
            try:
                rt[k] = [int(x) for x in v.split("|")] if k == "hist" else float(v)
            except ValueError:
                return None
        return {"rt": rt}
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
        self.rt = None                 # ultima linea RT
        self.markers = []              # (t, etiqueta) para las lineas verticales
        self.t_start_breath = None     # inicio de la respiracion en curso (deteccion por flujo)
        self.ti = None                 # duracion de la inspiracion en curso (s), al empezar la espiracion
        self.rr_meas = self.ie_meas = self.mv = None
        self.vts = collections.deque(maxlen=3)

    def _markers(self, d):
        """Lineas verticales: cambios de estado que el jurado tiene que ver en las curvas."""
        p, t = self.last, d["t"]
        if p is None or len(d) < 20:
            return
        if int(d["failovers"]) > int(p["failovers"]):
            self.markers.append((t, "breath_ctrl failed -> backup"))
        if int(d["hmi_pid"]) != int(p["hmi_pid"]):
            self.markers.append((t, "HMI restarted"))
        disc = 2
        if int(d["alarms"]) & disc and not int(p["alarms"]) & disc:
            self.markers.append((t, "disconnection"))
        if not int(d["alarms"]) & disc and int(p["alarms"]) & disc:
            self.markers.append((t, "reconnected"))
        if int(d["pwr_bits"]) & 1 and not int(p["pwr_bits"]) & 1:
            self.markers.append((t, "mains lost"))
        if not int(d["pwr_bits"]) & 1 and int(p["pwr_bits"]) & 1:
            self.markers.append((t, "mains restored"))
        if "set_pending" in d and p.get("set_pending") == 1 and d["set_pending"] == 0:
            self.markers.append((t, "settings applied"))

    def add(self, d, rx):
        if "caption" in d:
            self.caption = (d["caption"], rx)
            return
        if "rt" in d:
            self.rt = d["rt"]
            return
        self._markers(d)
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
                # FR e I:E medidas: periodo entre inicios de respiracion; Ti hasta que el flujo se vuelve
                # espiratorio. Ventilacion minuto = Vt medio de las ultimas respiraciones x FR medida.
                if self.t_start_breath is not None:
                    period = d["t"] - self.t_start_breath
                    if 1.0 < period < 12.0:
                        self.rr_meas = 60.0 / period
                        self.vts.append(self.vt)
                        self.mv = sum(self.vts) / len(self.vts) * self.rr_meas / 1e3
                        if self.ti and 0 < self.ti < period:
                            self.ie_meas = (period - self.ti) / self.ti
                self.t_start_breath = d["t"]
                self.ti = None
                self.v = 0.0
                self.breath = {"vmax": 0.0, "pmax": 0.0}
            # Histeresis: el contador solo se reinicia al superar el mismo umbral que dispara. Si se reiniciara
            # antes (entre 0.10 y 0.15 L/s), una subida muestreada justo ahi nunca dispararia.
            if d["flow"] > 0.15:
                self.neg_time = 0.0
            elif d["flow"] < 0.1:
                self.neg_time += dt
            self.v += d["flow"] * dt
            if self.ti is None and self.t_start_breath is not None and d["flow"] < -0.05:
                self.ti = d["t"] - self.t_start_breath
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
    out.append((f"SpO2   {spo2} %", col_spo2, 18))
    out.append((f"EtCO2  {d['etco2']:.1f} mmHg", INK, 12))
    out.append((f"HR     {d['hr']:.0f} bpm" + ("   (vitals stale)" if stale else ""), INK, 12))
    out.append(("", INK, 6))
    rt = st.rt or {}
    ibw = rt.get("ibw", 0)
    if st.vt is None:
        out.append(("Vt     --", INK, 12))
    elif ibw > 0:
        kg = st.vt / ibw
        col = CRIT if kg > VT_KG_HI else WARN if kg < VT_KG_LO else INK
        tag = "  [> 8: not protective]" if kg > VT_KG_HI else "  [< 6]" if kg < VT_KG_LO else ""
        out.append((f"Vt     {st.vt:.0f} mL  {kg:.1f} mL/kg{tag}", col, 12))
    else:
        out.append((f"Vt     {st.vt:.0f} mL", INK, 12))
    pk = "--" if st.ppeak is None else f"{st.ppeak:.1f} / {st.peep:.1f}"
    out.append((f"Ppeak / PEEP  {pk} cmH2O", INK, 11))
    rr = "--" if st.rr_meas is None else f"{st.rr_meas:.1f}"
    ie = "--" if st.ie_meas is None else f"1:{st.ie_meas:.1f}"
    mv = "--" if st.mv is None else f"{st.mv:.1f}"
    out.append((f"RR {rr} bpm  I:E {ie}  MV {mv} L/min", INK, 10))
    fio2 = rt.get("fio2", -1)
    out.append((f"FiO2 {fio2 * 100:.0f} %  (room air, fixed in Pulse)" if fio2 > 0 else "FiO2 --", INK, 10))
    ap = rt.get("ap", -1)
    if ap >= 0:
        lim = rt.get("lim_ap", 3)
        col = CRIT if int(d["alarms"]) >> 9 & 1 else WARN if ap > lim or rt.get("eef") else INK
        out.append((f"Auto-PEEP {ap:.1f} cmH2O (end-exp flow {abs(rt.get('qee', 0)) * 60:.1f} L/min)", col, 9))
    out.append((f"Patient R {d['r']:.1f} cmH2O·s/L  C {d['c_ml']:.0f} mL/cmH2O" +
                (f"  IBW {ibw:.1f} kg" if ibw > 0 else ""), MUTED, 8))
    if "p_insp" in d:
        mode = "VCV" if d["vcv"] else "PCV"
        main = f"Vt {d['vt_ml']:.0f} mL" if d["vcv"] else f"Pinsp {d['p_insp']:.0f}"
        tag = " (next)" if d["set_pending"] else ""
        out.append((f"Set {mode} {main} / PEEP {d['peep']:.0f} / {d['rr']:.0f} bpm / 1:{d['ie']:.0f}{tag}",
                    WARN if d["set_pending"] else INK, 9))
    if "lim_phi" in rt:
        out.append((f"Alarm limits: Paw > {rt['lim_phi']:.0f} | low Paw < {rt['lim_plo']:.0f} cmH2O | "
                    f"apnea {rt['lim_apnea']:.0f} s", MUTED, 8))
        out.append((f"  Vt {rt['lim_vtlo']:.0f}-{rt['lim_vthi']:.0f} mL | auto-PEEP > {rt['lim_ap']:.0f} cmH2O",
                    MUTED, 8))
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


def rt_color(ms):
    return GOOD if ms <= RT_OK_MS else WARN if ms <= RT_WARN_MS else CRIT


def rt_tag(ms):
    return "ok" if ms <= RT_OK_MS else "late" if ms <= RT_WARN_MS else "HIGH"


def rt_lines(st):
    """Panel de tiempo real: (texto, color, tamano)."""
    r = st.rt
    out = [("Real-time loop: breath_ctrl 1 kHz", INK, 10)]
    if st.last is not None and st.last["last_event"] == "MONO":
        return out + [("monolithic build: no loop metrics", MUTED, 9)]
    if not r or r.get("cyc", 0) <= 0:
        return out + [("waiting for loop metrics...", MUTED, 9)]
    cyc = r["cyc"]
    out.append((f"pid {int(r['pid'])}: {cyc:,.0f} cycles since it started".replace(",", " "), MUTED, 8))
    out.append((f"period p50   {r['p50']:7.3f} ms", GOOD if abs(r["p50"] - 1) < 0.05 else WARN, 10))
    for k, name in (("p99", "p99  "), ("p999", "p99.9"), ("max", "max  ")):
        out.append((f"period {name} {r[k]:7.2f} ms  [{rt_tag(r[k])}]", rt_color(r[k]), 10))
    miss = r["late"] / cyc * 100
    col = GOOD if miss < 0.1 else WARN if miss < 1 else CRIT
    out.append((f"deadline misses {r['late']:.0f} ({miss:.2f} %)", col, 10))
    out.append((f"lost periods    {r['lost']:.0f}", col, 10))
    out.append((f"compute p99 {r['ex99']:.0f} us  max {r['exmax'] / 1e3:.1f} ms", MUTED, 9))
    out.append(("command age at the plant (since start)", INK, 9))
    for k, name in (("age99", "p99"), ("agemax", "max")):
        out.append((f"  {name} {r[k]:7.2f} ms  [{rt_tag(r[k])}]", rt_color(r[k]), 10))
    if r.get("loop_age", 0) > 0.5:
        out.append((f"metrics {r['loop_age']:.1f} s old (controller restarting?)", WARN, 9))
    out.append((f"green <= {RT_OK_MS} ms, amber <= {RT_WARN_MS:.0f} ms, red above", MUTED, 7))
    out.append(("QNX 8.0 in a QEMU/KVM VM on WSL2 (Windows):", MUTED, 7))
    out.append(("host virtualization noise included (upper bound)", MUTED, 7))
    return out


def build_figure(plt):
    fig = plt.figure(figsize=(17, 8.6), facecolor=BG)
    fig.canvas.manager.set_window_title("BreathGuard RT") if hasattr(fig.canvas, "manager") and fig.canvas.manager \
        else None
    gs = fig.add_gridspec(3, 3, width_ratios=[2.35, 1.22, 1.0], hspace=0.28, wspace=0.06,
                          left=0.045, right=0.99, top=0.87, bottom=0.07)
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
    rt = fig.add_subplot(gs[:2, 2], facecolor=PANEL)
    for ax in (side, rt):
        ax.set_xticks([])
        ax.set_yticks([])
        for s in ax.spines.values():
            s.set_color(GRID)
    hist = fig.add_subplot(gs[2, 2], facecolor=PANEL)
    hist.set_xscale("log")
    hist.set_yscale("log")
    hist.tick_params(colors=MUTED, labelsize=7)
    for s in hist.spines.values():
        s.set_color(GRID)
    fig.text(0.05, 0.955, "BreathGuard RT  ·  QNX 8.0  ·  virtual patient: Pulse", color=INK, fontsize=13,
             weight="bold")
    clock = fig.text(0.985, 0.955, "", color=MUTED, fontsize=10, ha="right")
    caption = fig.text(0.05, 0.905, "", color=WARN, fontsize=13)
    return fig, axes, lines, (side, rt, hist), (clock, caption)


def draw_hist(ax, r):
    """Histograma del periodo del lazo (escala log en ambos ejes: la cola es lo que importa)."""
    ax.clear()
    ax.set_facecolor(PANEL)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_title("Loop period histogram", color=INK, fontsize=9, loc="left", pad=3)
    ax.tick_params(colors=MUTED, labelsize=7)
    for sp in ax.spines.values():
        sp.set_color(GRID)
    ax.set_xlim(HIST_EDGES[0], HIST_EDGES[-1])
    ax.set_xlabel("period (ms)", color=MUTED, fontsize=7, labelpad=1)
    ax.set_ylabel("cycles", color=MUTED, fontsize=7, labelpad=1)
    ax.grid(True, color=GRID, linewidth=0.5)
    ax.axvline(RT_OK_MS, color=WARN, linewidth=1, linestyle="--")
    ax.text(RT_OK_MS * 1.05, 0.97, "deadline", color=WARN, fontsize=7, va="top", transform=ax.get_xaxis_transform())
    if not r or "hist" not in r or not any(r["hist"]):
        return
    h = r["hist"]
    lefts, widths = HIST_EDGES[:-1], [b - a for a, b in zip(HIST_EDGES[:-1], HIST_EDGES[1:])]
    ax.bar(lefts, [x if x > 0 else 0 for x in h], width=widths, align="edge", color=C_PAW, edgecolor=PANEL,
           linewidth=1)
    ax.set_ylim(0.7, max(h) * 2)
    ax.set_xticks([0.5, 1, 2, 5, 10, 20])
    ax.set_xticklabels(["0.5", "1", "2", "5", "10", "20"])


CAPTION_S = 12.0  # segundos que se ve un subtitulo


_marker_artists = []
_last_hist = [None]


def draw(st, now_rx, lines, panels, texts, axes=None):
    side, rtax, hist = panels
    clock, caption = texts
    text, t_cap = st.caption
    caption.set_text(text if text and now_rx - t_cap < CAPTION_S else "")
    for a in _marker_artists:
        a.remove()
    _marker_artists.clear()
    if st.t:
        t_end = st.last["t"]
        xs = [x - t_end for x in st.t]
        for ln, ys in zip(lines, (st.paw, st.flow, st.vol)):
            ln.set_data(xs, list(ys))
        clock.set_text(f"QNX clock {t_end:9.2f} s")
        if axes:
            for tm, label in st.markers:
                x = tm - t_end
                if x < -WINDOW_S:
                    continue
                for ax in axes:
                    _marker_artists.append(ax.axvline(x, color=WARN, linewidth=1.2, linestyle="--"))
                _marker_artists.append(axes[0].text(x + 0.1, 40.5, label, color=WARN, fontsize=8, va="top"))
    for ax, content, x0 in ((side, status_lines(st, now_rx), 0.04), (rtax, rt_lines(st), 0.05)):
        for txt in list(ax.texts):
            txt.remove()
        y = 0.975
        for text, col, size in content:
            ax.text(x0, y, text, color=col, fontsize=size, va="top", family="monospace", transform=ax.transAxes)
            y -= 0.009 + size * (0.0034 if ax is side else 0.0054)
    key = tuple(st.rt["hist"]) if st.rt and "hist" in st.rt else None
    if key != _last_hist[0]:  # redibujar el histograma solo cuando cambia (1 Hz)
        _last_hist[0] = key
        draw_hist(hist, st.rt)


def read_csv(path):
    op = gzip.open if path.endswith(".gz") else open
    rows = []
    with op(path, "rt") as f:
        next(f)
        for raw in f:
            host, _, rest = raw.partition(",")
            d = parse(rest)
            if d and (rows or ("caption" not in d and "rt" not in d)):  # el inicio lo da la telemetria
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
        fig, axes, lines, panels, clock = build_figure(plt)
        draw(st, t0 + a.at, lines, panels, clock, axes)
        fig.savefig(a.snapshot, dpi=110, facecolor=BG)
        print(f"dashboard: {a.snapshot} (t = {a.at:.1f} s)")
        return

    fig, axes, lines, panels, clock = build_figure(plt)
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
        draw(st, poll(), lines, panels, clock, axes)
        return []

    anim = FuncAnimation(fig, update, interval=100, cache_frame_data=False)  # noqa: F841 (debe vivir)
    plt.show()


if __name__ == "__main__":
    main()
