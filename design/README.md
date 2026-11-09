# Design package: reference data for an independent model

This folder lets you build a model of BreathGuard RT outside this code base, for example in MATLAB/Simulink, and
check it against the system running on QNX.

| File | Contents |
|---|---|
| [plant_model.md](plant_model.md) | Equations and parameter values of the pneumatic plant and lung, the integration method, the delays, and the coupling with Pulse |
| [controller.md](controller.md) | The control law of `breath_ctrl` as implemented: schedule, PI with feedforward, gains, discretization, saturation, anti-windup, phases |
| `ref_pcv_qnx.csv.gz` | PCV 20/5 cmH2O, 12 bpm, I:E 1:2, 60 s at 1 kHz, recorded on QNX 8.0 (`pneumatic_sim` + `breath_ctrl`, fixed RC, no Pulse) |
| `ref_pcv_offline.csv.gz` | Same scenario from the same C code on the host with an exact 1 ms clock (`tools/design_ref.c`) |
| `step_qnx.csv.gz` | Open-loop step test on the plant, recorded on QNX (`src/step_test.c`) |
| `step_offline.csv.gz` | Same step test on the host |
| `compare.txt` | QNX vs ideal: RMS and maximum differences |

Regenerate everything with `scripts/design_run.sh`, which needs the QNX VM running. The files are gzip-compressed;
in MATLAB, `gunzip('ref_pcv_qnx.csv.gz'); T = readtable('ref_pcv_qnx.csv');`.

## Columns

One row per control cycle. On QNX that is one row per actual cycle, so the time step is the measured period and is
not exactly 1 ms.

| Column | Unit | Meaning |
|---|---|---|
| `t_s` | s | time from the start of the first breath (QNX: first controller cycle) |
| `phase` | – | 0 inspiration, 1 expiration, 2 inspiratory pause (VCV only); −1 in the step files |
| `setpoint` | cmH2O | PCV pressure setpoint: the ramp to Pinsp, then PEEP during expiration |
| `paw_meas`, `flow_meas` | cmH2O, L/s | what the controller read: the true values plus sensor noise |
| `blower_cmd` | 0–1 | blower command `u_b`; blower pressure target = 60 × `u_b` cmH2O |
| `exh_cmd` | 0–1 | exhalation valve opening `u_e` (0 closed, 1 open) |
| `peep_cmd` | cmH2O | PEEP valve setting (5) |
| `paw_true`, `flow_true` | cmH2O, L/s | true plant state at the instant of the read |
| `volume_ml` | mL | lung volume above its rest volume at PEEP 5 |
| `pbl` | cmH2O | blower pressure (plant state) |
| `palv` | cmH2O | alveolar pressure (plant state) |
| `period_ms`, `exec_us` | ms, µs | QNX files only: measured loop period and computation time of the cycle |

On QNX the true state comes from a different process (the plant logs every 1 ms tick). It is linearly
interpolated to the instant of each controller cycle.

## Step test (system identification)

The plant runs alone, with no controller. The commands follow a fixed schedule:

| Time | Blower | Exhalation valve | What it excites |
|---|---|---|---|
| 0–1 s | 5 cmH2O (`u_b` = 0.0833) | closed | rest at PEEP 5 |
| 1–10 s | **20 cmH2O** (`u_b` = 0.3333) | closed | blower lag (40 ms), tubing, and lung filling through R_in + R_pat (tau ≈ 1.47 s) |
| 10–16 s | 5 cmH2O | **open**, PEEP valve 5 | passive expiration through R_pat + R_ex (tau ≈ 1.42 s) |

Useful identifications:
- `blower_cmd` → `paw_true` and `flow_true` during 1–10 s;
- the `flow_true` decay after 10 s.

The check valve and the PEEP valve make the plant nonlinear. A linear model is valid only within one phase.

## Suggested Simulink setup

- Plant: continuous states `Pbl`, `Paw`, `V`. Use fixed-step `ode1` (Euler) at 50 µs to match the C code exactly,
  or `ode4` for a better reference. The check valve and the PEEP valve are `max(·, 0)` terms.
- Controller: a triggered or atomic subsystem with a 1 ms sample time and zero-order hold on the outputs. The
  integral is reset at the start of each breath; conditional integration as in `controller.md`.
- Noise: compare against `paw_true`/`flow_true`. To reproduce the controller's actual inputs, feed
  `paw_meas`/`flow_meas` from the file. The noise sequence on QNX depends on how many times other processes read
  the sensors.
- Expected agreement with `ref_pcv_offline.csv.gz`: within numerical precision when using ode1 at 50 µs and
  feeding the recorded `paw_meas`/`flow_meas` to the controller. With your own noise, or without noise, expect
  small differences: the noise has a standard deviation of 0.15 cmH2O and 0.01 L/s.
  Against `ref_pcv_qnx.csv.gz`, see `compare.txt`: in this run the RMS differences were 0.09 cmH2O and 0.4 mL.
