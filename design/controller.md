# Controller: `breath_ctrl`

The control law as implemented in `src/vent.c` (shared by `breath_ctrl`, the backup ventilator in `safety_mon`, and
`mono_vent`), plus the loop timing in `src/breath_ctrl.c`. Units as in [plant_model.md](plant_model.md).

## 1. Loop timing

- A QNX kernel timer (`timer_create` with `SIGEV_PULSE`, period 1 ms) sends a pulse to the loop's private channel.
  The loop runs at priority 50 with `SCHED_FIFO`.
- Each cycle k, after the pulse:
  1. `now = ClockCycles()` in seconds; `dt = now - now_prev` (the **measured** period, not 1 ms).
  2. `MsgSend(READ)` to the plant returns `Paw_meas`, `flow_meas`. The plant integrates up to the arrival
     instant first.
  3. `vent_step(now, dt, sensors)` returns `u_b`, `u_e`, `P_peep`.
  4. `MsgSend(WRITE)` applies the command at its arrival instant, held until the next WRITE.
  5. Every 10 cycles, a heartbeat pulse goes to `safety_mon`. Every 100 cycles, a bounded settings query
     (`TimerTimeout`, 2 ms) also carries the loop metrics.
- The breath schedule uses the **clock**, not a cycle counter: a late cycle does not stretch the breath.

Discrete-time model for a reference implementation: a sample time of 1 ms, sensors sampled at `t_k`, outputs
applied at `t_k` (the real delay is the computation time, 5–10 µs median) and held for 1 ms.

## 2. Breath schedule

Settings: `rr` (bpm), `ie` (Te/Ti, so 2 means 1:2), `p_insp`, `peep` (cmH2O), `rise_s` = 0.1 s; for VCV also `vt`
(L) and `p_limit` = 40 cmH2O.

    T     = 60 / rr                         breath period
    Ti    = T / (1 + ie)                    inspiratory time
    n     = floor((t - t0) / T)             breath number
    tb    = (t - t0) - n * T                time into the breath

`t0` is the start of the first breath. `safety_mon` sets it to its start time + 0.5 s and passes it to every
controller it launches (`-T`). At the start of each breath (when n changes) the integral term is reset to 0 and the
pressure-limit flag is cleared.

Phase: inspiration if `tb < Ti` (and the breath was not cut by the pressure limit), otherwise expiration.

## 3. Expiration (both modes)

Open loop:

    u_b    = peep / Pbl_max                 blower idles at PEEP (the check valve keeps it out of the circuit)
    u_e    = 1                              exhalation valve open
    P_peep = peep                           PEEP valve set to PEEP
    setpoint = peep (PCV) or 0 (VCV, flow)

## 4. PCV inspiration (pressure control)

    u_e    = 0,  P_peep = peep
    ramp   = sat(tb / rise_s, 0, 1)
    r      = peep + (p_insp - peep) * ramp                 pressure setpoint (cmH2O)
    e      = r - Paw_meas
    hi     = sat(p_insp + 8, 0, Pbl_max)                    blower cap ("pressure-limited source")
    PI with feedforward ff = r, Kp = 2.0, Ki = 15.0 1/s:
        v  = ff + Kp*e + I                                  (I from the previous cycle)
        if (v < hi or e < 0) and (v > 0 or e > 0):          conditional integration (anti-windup)
            I = I + Ki * e * sat(dt, 0, 0.01)
        Pbl_cmd = sat(ff + Kp*e + I, 0, hi)                 (with the updated I)
    u_b    = Pbl_cmd / Pbl_max

Notes:
- Type: parallel PI on the pressure error, with the setpoint as feedforward. There is no derivative term.
- Discretization: the integral is a forward rectangle, `I_k = I_{k-1} + Ki e_k dt_k`, using the current error. The
  output uses the updated `I_k`. `dt_k` is the measured period, clipped to 10 ms so a long VM pause cannot make the
  integral jump.
- Anti-windup: the integral freezes while the unclipped output is at a limit and the error pushes it further. The
  same cap `hi` is used for the clip and the anti-windup test.
- The cap `p_insp + 8` was added after the demo found a 44 cmH2O spike on tube reconnection (see `docs/demo.md`).
  A normal breath needs 2–3 cmH2O above `p_insp`.

## 5. VCV inspiration (volume control)

    u_e    = 0,  P_peep = peep
    t_flow = 0.8 * Ti                                       flow phase; the last 20 % is an inspiratory pause
    if Paw_meas > p_limit: end inspiration now (limited = 1, expiration for the rest of the breath)
    if tb < t_flow:
        q_set = vt / (t_flow - rise_s / 2)                  compensates the volume lost in the ramp
        r     = q_set * sat(tb / rise_s, 0, 1)              flow setpoint (L/s)
        e     = r - flow_meas
        ff    = Paw_meas + r * R_in_est,  R_in_est = 3.0    pressure needed to push r through the inspiratory branch
        same PI structure with Kq = 20 cmH2O/(L/s), Kiq = 50 cmH2O/L, hi = Pbl_max
        u_b   = Pbl_cmd / Pbl_max
    else (pause):
        u_b   = 0  (the check valve holds the volume), setpoint 0

## 6. Settings changes

Operator changes are validated by `safety_mon` and applied at the start of a breath (`t0` of the new settings).
At that instant `breath_ctrl` re-initializes the controller (integral 0, new schedule). This does not happen in the
reference data.

## 7. What `safety_mon` adds on top (not part of the control law)

- High-pressure relief: if Paw > 40 cmH2O for 3 ms, it overrides the actuators. The exhalation valve opens and the
  blower drops to PEEP until the next breath.
- Backup ventilation: on a controller failure, the same `vent_step` code runs inside `safety_mon` with the same
  settings and `t0`, and the plant obeys it (override).

## 8. Reference data in this folder

| File | What |
|---|---|
| `ref_pcv_qnx.csv.gz` | QNX 8.0 VM, `pneumatic_sim` + `breath_ctrl`, PCV 20/5, 12 bpm, I:E 1:2, fixed RC (no Pulse), 60 s at 1 kHz |
| `ref_pcv_offline.csv.gz` | Same equations on the host with an ideal 1 ms clock (`tools/design_ref.c`): no timing jitter |
| `step_offline.csv.gz` | Open-loop steps on the plant alone, for system identification (see `README.md`) |
| `step_qnx.csv.gz` | Same steps sent to `pneumatic_sim` on QNX by `step_test` |

Comparing your Simulink model with the offline file checks the equations, because both should agree to numerical
precision. Comparing it with the QNX file adds the real-time effects: measured `dt`, late cycles and VM noise.
