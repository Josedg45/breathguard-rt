"""dashboard_controls.py - interactive panel of the dashboard (dashboard.py --control).

Two groups, kept apart on screen and in code, because they are different things in a real device:
  OPERATOR   settings and copilot answers. UDP to the HMI -> safety_mon validates (bench.send_operator, the same
             path as bg_cmd.py). An invalid value comes back as NAK with the reason.
  TEST BENCH patient type, faults, mains, killing processes. SSH to scripts/bench_target.sh (bench.bench): the
             physical world and the test engineer, never through the HMI.
Every action runs in a worker thread, so a slow SSH or a dead HMI never freezes the curves.
"""
import threading

from matplotlib.widgets import Button, RadioButtons, TextBox

import bench
from patients import HINT, IDS, LABEL

INK, MUTED, PANEL, GRID = "#ecebe7", "#9a9993", "#222220", "#3a3a37"
GOOD, WARN, CRIT = "#0ca30c", "#fab219", "#d03b3b"
BTN, BTN_HOVER, BTN_ON = "#2e2e2b", "#3d3d39", "#6b4a12"

# Fault values the scenes already measured (scripts/c5_target.sh, demo_run.sh).
LEAK_G = 0.004      # L/s per cmH2O
OBSTRUCTION = 3.0   # x patient resistance


class Controls:
    def __init__(self, fig, st, x0=0.808, x1=0.995):
        self.fig, self.st = fig, st
        self.busy = {"op": False, "bench": False}
        self.msg = {"op": ("Operator: settings go to safety_mon, which validates them.", MUTED),
                    "bench": ("Bench: start it with scripts/bench.py start (or bench_run.sh).", MUTED)}
        self.faults = {"d": 0, "l": 0.0, "o": 1.0, "k": 0}
        self.mains_on = True
        self.filled = False      # text boxes filled from telemetry once
        self.widgets = []        # keep references (matplotlib widgets die if not referenced)
        w = x1 - x0
        y = 0.955

        def header(text, yy):
            fig.text(x0, yy, text, color=INK, fontsize=10, weight="bold")

        def axes(yy, h, xx=x0, ww=w):
            ax = fig.add_axes([xx, yy, ww, h], facecolor=PANEL)
            return ax

        def button(label, yy, cb, xx=x0, ww=w, h=0.03):
            b = Button(axes(yy, h, xx, ww), label, color=BTN, hovercolor=BTN_HOVER)
            b.label.set_color(INK)
            b.label.set_fontsize(8.5)
            b.on_clicked(lambda _e: cb())
            self.widgets.append(b)
            return b

        # ---------------- OPERATOR ----------------
        header("OPERATOR  (HMI -> safety_mon validates)", y)
        y -= 0.045
        self.mode = RadioButtons(axes(y, 0.035, x0, w * 0.42), ("PCV", "VCV"), active=0, activecolor=WARN)
        for t in self.mode.labels:
            t.set_color(INK)
            t.set_fontsize(9)
        self.widgets.append(self.mode)
        fields = [("p_insp", "Pinsp", "20"), ("peep", "PEEP", "5"), ("rr", "RR", "12"), ("ie", "I:E 1:", "2"),
                  ("vt", "Vt mL", "500"), ("fio2", "FiO2 %", "21")]
        self.boxes = {}
        col_w = w * 0.5
        for i, (key, label, val) in enumerate(fields):
            r, c = divmod(i, 2)
            yy = y - 0.045 - r * 0.04
            xx = x0 + c * col_w + w * 0.2
            tb = TextBox(axes(yy, 0.03, xx, col_w - w * 0.22), label + " ", initial=val, color=BTN,
                         hovercolor=BTN_HOVER, label_pad=0.04)
            tb.label.set_color(MUTED)
            tb.label.set_fontsize(8)
            tb.text_disp.set_color(INK)
            tb.text_disp.set_fontsize(9)
            self.boxes[key] = tb
            self.widgets.append(tb)
        y -= 0.045 + 3 * 0.04 + 0.005
        button("Apply settings (next breath)", y, self.apply_settings)
        y -= 0.036
        button("Reload from ventilator", y, self.reload_settings, ww=w * 0.49)
        button("Accept proposal", y, lambda: self.copilot("accept"), xx=x0 + w * 0.51, ww=w * 0.49)
        y -= 0.036
        button("Reject proposal", y, lambda: self.copilot("reject"), xx=x0 + w * 0.51, ww=w * 0.49)
        self.op_text = fig.text(x0, y - 0.006, "", color=MUTED, fontsize=8, va="top", wrap=True)
        y -= 0.075

        # ---------------- TEST BENCH ----------------
        header("TEST BENCH  (physical world, via SSH)", y)
        y -= 0.012
        h = 0.026 * len(IDS)
        y -= h
        self.patient = RadioButtons(axes(y, h), [LABEL[p] for p in IDS], active=0, activecolor=WARN)
        for t in self.patient.labels:
            t.set_color(INK)
            t.set_fontsize(8.5)
        self.patient.on_clicked(lambda _l: self.show_hint())
        self.widgets.append(self.patient)
        y -= 0.036
        button("Connect this patient", y, self.load_patient)
        self.hint = fig.text(x0, y - 0.006, "", color=MUTED, fontsize=7.5, va="top", wrap=True)
        y -= 0.06
        half = w * 0.49
        self.b_disc = button("Disconnect tube", y, lambda: self.toggle("d"), ww=half)
        self.b_leak = button("Leak", y, lambda: self.toggle("l"), xx=x0 + w * 0.51, ww=half)
        y -= 0.036
        self.b_obst = button(f"Obstruction x{OBSTRUCTION:g}", y, lambda: self.toggle("o"), ww=half)
        self.b_stuck = button("P sensor stuck", y, lambda: self.toggle("k"), xx=x0 + w * 0.51, ww=half)
        y -= 0.036
        button("Clear all faults", y, self.clear_faults, ww=half)
        self.b_mains = button("Mains power OFF", y, self.toggle_mains, xx=x0 + w * 0.51, ww=half)
        y -= 0.036
        button("Kill breath_ctrl", y, lambda: self.run_bench("kill", "ctrl"), ww=half)
        button("Kill HMI", y, lambda: self.run_bench("kill", "hmi"), xx=x0 + w * 0.51, ww=half)
        self.bench_text = fig.text(x0, y - 0.006, "", color=MUTED, fontsize=8, va="top", wrap=True)
        self.show_hint()

    # ---------------- helpers ----------------
    def _worker(self, kind, fn, *args):
        if self.busy[kind]:
            self.msg[kind] = ("busy: previous action still running", WARN)
            return
        self.busy[kind] = True
        self.msg[kind] = ("...", MUTED)

        def run():
            try:
                text, col = fn(*args)
            except Exception as e:  # a bad SSH/UDP must not kill the dashboard
                text, col = f"error: {e}", CRIT
            self.msg[kind] = (text, col)
            self.busy[kind] = False

        threading.Thread(target=run, daemon=True).start()

    def _op(self, cmd):
        ans = bench.send_operator(cmd)
        if ans.startswith("ACK"):
            self.filled = False  # refill the boxes from what the ventilator now holds
        return f"{cmd}\n-> {ans}", GOOD if ans.startswith("ACK") else CRIT

    def _bench(self, *args):
        ok, out = bench.bench(*args)
        return out.splitlines()[-1] if out else ("ok" if ok else "failed"), GOOD if ok else CRIT

    def run_bench(self, *args):
        self._worker("bench", self._bench, *args)

    # ---------------- operator ----------------
    def apply_settings(self):
        vals = {}
        for k, tb in self.boxes.items():
            txt = tb.text.strip().replace(",", ".")
            try:
                vals[k] = float(txt)
            except ValueError:
                self.msg["op"] = (f"'{txt}' is not a number ({k})", CRIT)
                return
        vcv = self.mode.value_selected == "VCV"
        parts = [f"mode={'vcv' if vcv else 'pcv'}", f"peep={vals['peep']:g}", f"rr={vals['rr']:g}",
                 f"ie={vals['ie']:g}", f"fio2={vals['fio2']:g}"]
        parts.append(f"vt={vals['vt']:g}" if vcv else f"p_insp={vals['p_insp']:g}")
        self._worker("op", self._op, " ".join(parts))

    def copilot(self, verb):
        c = self.st.cp or {}
        pid_ = int(c.get("id", 0) or 0)
        if not pid_ or c.get("state") != "pending":
            self.msg["op"] = ("no pending copilot proposal", WARN)
            return
        self._worker("op", self._op, f"copilot {verb} {pid_}")

    def reload_settings(self):
        self.filled = False

    def fill_from(self, d, rt):
        """Text boxes <- the ventilator's current settings (telemetry), once, and again after each ACK."""
        if self.filled or d is None or "p_insp" not in d:
            return
        vals = {"p_insp": d["p_insp"], "peep": d["peep"], "rr": d["rr"], "ie": d["ie"], "vt": d["vt_ml"]}
        if rt and isinstance(rt.get("set_fio2"), float) and rt["set_fio2"] > 0:
            vals["fio2"] = rt["set_fio2"] * 100
        for k, v in vals.items():
            self.boxes[k].set_val(f"{v:g}")
        self.mode.set_active(1 if d["vcv"] else 0)
        self.filled = True

    # ---------------- bench ----------------
    def selected_patient(self):
        return IDS[[LABEL[p] for p in IDS].index(self.patient.value_selected)]

    def show_hint(self):
        self.hint.set_text(HINT[self.selected_patient()])

    def load_patient(self):
        self._worker("bench", self._bench, "patient", self.selected_patient())

    def _send_faults(self, before):
        """Send the whole fault set (the plant replaces it at once). If the command fails, the buttons go back to
        what the plant still has, so the panel never shows a fault that was not injected."""
        f = dict(self.faults)

        def run():
            text, col = self._bench("fault", "-d", f["d"], "-l", f["l"], "-o", f["o"], "-k", f["k"])
            if col != GOOD:
                self.faults = before
            return text, col

        self._worker("bench", run)

    def toggle(self, key):
        if self.busy["bench"]:
            self.msg["bench"] = ("busy: previous action still running", WARN)
            return
        before = dict(self.faults)
        f = self.faults
        if key == "d":
            f["d"] = 0 if f["d"] else 1
        elif key == "l":
            f["l"] = 0.0 if f["l"] else LEAK_G
        elif key == "o":
            f["o"] = 1.0 if f["o"] > 1 else OBSTRUCTION
        elif key == "k":
            f["k"] = 0 if f["k"] else 1
        self._send_faults(before)

    def clear_faults(self):
        before = dict(self.faults)

        def run():
            text, col = self._bench("fault")
            self.faults = {"d": 0, "l": 0.0, "o": 1.0, "k": 0} if col == GOOD else before
            return text, col

        self._worker("bench", run)

    def toggle_mains(self):
        if self.busy["bench"]:
            self.msg["bench"] = ("busy: previous action still running", WARN)
            return
        want = not self.mains_on

        def run():
            text, col = self._bench("mains", 1 if want else 0)
            if col == GOOD:
                self.mains_on = want
            return text, col

        self._worker("bench", run)

    # ---------------- every frame ----------------
    def refresh(self, now_rx):
        self.fill_from(self.st.last, self.st.rt)
        f = self.faults
        for b, on in ((self.b_disc, f["d"]), (self.b_leak, f["l"] > 0), (self.b_obst, f["o"] > 1),
                      (self.b_stuck, f["k"]), (self.b_mains, not self.mains_on)):
            b.color = BTN_ON if on else BTN
            b.ax.set_facecolor(b.color)
        self.b_mains.label.set_text("Mains power ON" if not self.mains_on else "Mains power OFF")
        rt = self.st.rt or {}
        name = rt.get("patient", "-")
        connected = f"Connected patient: {LABEL.get(name, name)}" if isinstance(name, str) and name != "-" \
            else "Connected patient: none (Pulse not running)"
        text, col = self.msg["bench"]
        self.bench_text.set_text(f"{connected}\n{text}")
        self.bench_text.set_color(col if text != "..." else MUTED)
        text, col = self.msg["op"]
        self.op_text.set_text(text)
        self.op_text.set_color(col)
