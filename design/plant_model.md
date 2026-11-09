# Plant model: `pneumatic_sim`

These are the exact equations and parameter values that BreathGuard RT runs on QNX. They come from `src/plant.c` (the
physics), `src/pneumatic_sim.c` (real-time integration and the Pulse coupling) and `src/bg_types.h` (signals). This
document is meant for building an independent reference model, for example in Simulink, and comparing it with the
QNX runs in this folder.

Units everywhere: pressure cmH2O relative to atmosphere, volume L, flow L/s, resistance cmH2O/(L/s), compliance
L/cmH2O, time s.

## 1. Structure

```
 blower --R_in--|>|--+-- [Y-piece, node Paw, C_tube] --R_pat--> lung (C_pat, V0)
 (first order)  check|
               valve +--> exhalation valve (opening u_e) + PEEP valve (P_peep) --R_ex--> atmosphere
```

States (3): blower pressure `Pbl`, Y-piece pressure `Paw`, absolute lung volume `V`.
Inputs (3, from the controller): `u_b` blower command in [0, 1]; `u_e` exhalation valve opening in [0, 1];
`P_peep` PEEP valve opening pressure (cmH2O).

## 2. Equations

Alveolar pressure (linear single-compartment lung):

    Palv = (V - V0) / C_pat

Blower (first-order lag towards the commanded pressure):

    dPbl/dt = (Pbl_max * sat(u_b, 0, 1) - Pbl) / tau_bl

Inspiratory branch with an ideal check valve (flow only from blower to Y):

    q_in = max(Pbl - Paw, 0) / R_in

Exhalation valve in series with an ideal threshold PEEP valve (air leaves only above P_peep):

    q_ex = sat(u_e, 0, 1) * max(Paw - P_peep, 0) / R_ex

Leak fault (0 in all reference data): `q_leak = g_leak * Paw`.

Patient branch, tube connected (normal case):

    q_y    = (Paw - Palv) / R_pat          flow into the patient at the Y-piece
    q_lung = q_y

Tube disconnected (fault, not used in the reference data):

    q_y    = Paw / R_disc                  the circuit empties to the room
    q_lung = -Palv / R_pat                 the lung exhales to the room

Node balances:

    dPaw/dt = (q_in - q_ex - q_leak - q_y) / C_tube
    dV/dt   = q_lung

Obstruction fault: `R_pat` is multiplied by the obstruction factor (1 = normal).

Initial state: `Pbl = Paw = PEEP_0`, `V = V0 + C_pat * PEEP_0` (lung at rest at the initial PEEP). Until the
controller writes its first command, the actuators are `u_b = PEEP_0 / Pbl_max`, `u_e = 1`, `P_peep = PEEP_0`.

## 3. Parameters (`plant_default_params()`)

| Symbol | Code | Value | Unit | Meaning |
|---|---|---|---|---|
| Pbl_max | `pbl_max` | 60.0 | cmH2O | blower pressure at `u_b = 1` |
| tau_bl | `tau_bl` | 0.040 | s | blower time constant |
| R_in | `r_in` | 3.0 | cmH2O/(L/s) | inspiratory branch |
| C_tube | `c_tube` | 0.0015 | L/cmH2O | tubing compliance (1.5 mL/cmH2O) |
| R_ex | `r_ex` | 2.0 | cmH2O/(L/s) | expiratory branch, valve fully open |
| R_disc | `r_disc` | 0.5 | cmH2O/(L/s) | open Y-piece when disconnected |
| R_pat | `r_pat` | 22.4 | cmH2O/(L/s) | patient airway (endotracheal tube included) |
| C_pat | `c_pat` | 0.058 | L/cmH2O | patient respiratory compliance (58 mL/cmH2O) |
| V0 | `v0` | 2.3 | L | lung volume at Palv = 0 |
| sigma_paw | `noise_paw` | 0.15 | cmH2O | pressure sensor noise (std) |
| sigma_flow | `noise_flow` | 0.01 | L/s | flow sensor noise (std) |
| g_leak | fault | 0 | L/s per cmH2O | leak conductance |

`R_pat` and `C_pat` are the values Pulse reports for its StandardMale patient after ventilation at PCV 20/5 (M2).
Derived time constants:

- Tubing: `C_tube * (R_in || R_ex || R_pat)`, about 1.7 ms. This is why the integrator uses 50 µs substeps.
- Passive expiration: `(R_pat + R_ex) * C_pat = 24.4 * 0.058 = 1.415 s`.
- Inspiration from the blower: `(R_in + R_pat) * C_pat = 1.47 s` (PCV does not equilibrate in Ti = 1.67 s).

## 4. Sensors

Read when the controller sends `BG_MSG_READ`, at the instant the message arrives (the physics is first integrated
up to that instant):

    Paw_meas  = Paw + sigma_paw * n1
    flow_meas = q_y + sigma_flow * n2

`n1` and `n2` are standard normal values from a deterministic generator (xorshift64 seeded with
`0x9E3779B97F4A7C15`, then Box-Muller with one cosine branch). The flow is the flow at the Y-piece towards the
patient, positive on inspiration. Sensor fault `paw_stuck`: `Paw_meas` keeps its last value.

## 5. Numerical integration

- Explicit (forward) Euler with a fixed **50 µs** substep. A remaining interval shorter than 50 µs is integrated in
  one shorter step.
- Real time on QNX: the plant has no fixed clock of its own. Before handling any event (a 1 ms timer pulse, a
  READ, a WRITE, an OVERRIDE, a FAULT) it integrates from the previous event up to `now`, measured with
  `ClockCycles()`. Commands are therefore held (zero-order hold) between WRITE messages and applied at the exact
  instant they arrive. A gap longer than 0.1 s is clipped to 0.1 s (it never happened in the reference runs).
- The 1 ms timer pulse only advances the physics and logs one sample (`sim_*.csv`).

## 6. Delays in the loop

There is no transport delay inside the plant. The delays come from the loop around it:

| Stage | Delay |
|---|---|
| Sampling to command | `breath_ctrl` reads the sensors and writes the command within one cycle. The plant applies the command 5–10 µs (median) after the sample it was computed from. Under VM noise this can reach milliseconds; see `exec_us` in the QNX CSV. |
| Command hold | Zero-order hold until the next WRITE, about 1 ms. |
| Blower | First-order lag, tau_bl = 40 ms. This dominates the dynamics seen by the pressure loop. |

For a discrete reference model, use sample and hold at 1 kHz with no extra delay. You can add a 10 µs delay if
you want to reproduce the computation time.

## 7. Coupling with Pulse (the virtual patient)

Pulse advances in fixed 20 ms steps and does not run at 1 kHz. The coupling is a co-simulation at 50 Hz:

1. The plant cuts its time into 20 ms windows. For each window it computes the mean pressure at the patient's
   mouth, which is `Paw` while connected and 0 while disconnected (`paw_int` in `plant.c`).
2. `patient` (Pulse) applies that mean pressure as a ventilator pressure for one 20 ms step. It returns the lung
   volume `V_P`, the flow `Q_P`, and its resistance `R_P` and compliance `C_P`.
3. When the result for window k arrives, a few milliseconds late, the plant:
   - corrects its current lung volume by `dV = V_P - V(end of window k)`;
   - shifts the stored end volumes of the windows already closed after k by the same `dV`;
   - sets `R_pat = R_P`, `C_pat = C_P`;
   - sets `V0 = V_P - C_P * (paw_mean_k - Q_P * R_P)`, so the RC lung has the same alveolar pressure as Pulse.
4. The 1 kHz physics in between uses the RC model with those parameters.

Pulse's R and C are breath averages. In the demo they stay near R = 22 cmH2O·s/L and C = 47–68 mL/cmH2O (C depends
on pressure). Volume corrections are a few mL per window (see M2 and M3 in `PROGRESS.md`).

**The reference CSV files in this folder were recorded without Pulse, with the parameters of section 3 held
constant.** Only that case can be reproduced exactly by an independent model. A run with Pulse adds the corrections
above, which arrive at times set by Pulse's computation on the VM.

## 8. Expected values for the reference scenario

PCV, Pinsp 20, PEEP 5, 12 bpm, I:E 1:2 (Ti 1.667 s, Te 3.333 s), rise time 0.1 s, fixed RC:

| Quantity | Value | Source |
|---|---|---|
| Tidal volume | 576 mL | offline, steady state |
| Peak airway pressure | 20.4 cmH2O | 0.4 cmH2O overshoot at the end of the rise |
| Peak inspiratory flow | 0.61 L/s (36 L/min) | |
| End-inspiratory flow | 0.18 L/s | not zero: PCV does not equilibrate |
| End-expiratory flow | −0.043 L/s (−2.6 L/min) | not zero either: auto-PEEP |
| Auto-PEEP (true) | 1.06 cmH2O | `tools/lung_mon_test.c` |

The volume column in the CSV files is `V - (V0 + C_pat * 5)`, the lung volume above its resting volume at PEEP 5,
in mL. It does not start at exactly zero, because the breath before the first one is not complete.
