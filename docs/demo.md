# BreathGuard RT demo

One 3-minute run of the complete system on QNX 8.0 (QEMU x86_64 VM) with the Pulse virtual patient. The host
triggers each scene over SSH at a fixed time and sends a caption to the dashboard, so the screen recording and the
events stay in sync. Everything received is recorded and can be replayed.

```bash
scripts/demo_run.sh            # opens the live dashboard and records results/demo/hmi_<stamp>.csv
scripts/demo_run.sh --no-gui   # same run, headless (CSV only)
python3 scripts/dashboard.py --replay results/demo/hmi_20261109_091447.csv.gz   # replay the reference run
python3 scripts/m6_settings_check.py results/demo/sim_20261109_091447.csv.gz    # breath-by-breath table
```

## Timeline and measured results

Reference run: `results/demo/*_20261109_091447*` (PCV 20/5 cmH2O, 12 bpm, room air). Times are seconds from the
start of `safety_mon`; the dashboard caption appears about 3 s earlier.

| t (s) | Scene | What happens (measured) |
|---|---|---|
| 0–19 | Stable PCV | Vt 569 mL, Ppeak 20.5, PEEP 5.1, SpO2 98 % |
| 19 | `kill -9` of the HMI | `safety_mon` relaunches it; breaths before and after identical (Vt 569 mL) |
| 35–55 | Junk load on both CPUs (priority 9, 20 s) | Vt 569 mL every breath; plant command age stays ≤ 13 ms (median 1.3 ms) |
| 59 | Tube disconnected | DISCONNECTION alarm 21.1 ms after the fault; SpO2 dips to 96.3 % over 12 s |
| 71 | Tube reconnected (during an inspiration) | Peak 23.3 cmH2O, no overshoot (see below) |
| 85 | `kill -9 breath_ctrl` | Backup ventilator in control 0.18 ms after detection; new controller takes back at the next breath start |
| 105 | Operator: Pinsp 20 → 25 | Validated by `safety_mon`, applied exactly at the 105.5 s breath; Vt 767 mL |
| 122–157 | Mains power lost (battery time ×60) | Detected in 1.9 ms; load shedding; autonomy shown; ventilation unchanged |

End of run: SpO2 98.5 %, 0 Pulse windows lost.

## A bug the test bench found

The first run of this demo (`results/demo/*_20261109_090520*`) reconnected the tube in the middle of an inspiration
and the airway pressure jumped to **44 cmH2O** in 3 ms. The relief valve in `safety_mon` opened at 40 cmH2O, as
designed, but the controller should never have caused it. Cause: with the tube open, the pressure at the Y-piece
cannot rise, so the PCV loop drove the blower to its maximum (60 cmH2O); on reconnection that pressure reached the
patient at once.

Fix (`src/vent.c`): in PCV the blower is a pressure-limited source, capped at `p_insp + 8 cmH2O` (a normal breath
needs 2–3 cmH2O above `p_insp`), and the anti-windup uses the same cap. Offline the reconnection peak drops from 44.1
to 23.5 cmH2O with identical Vt for normal lungs (only very compliant, unrealistic lungs lose up to 3 % of Vt). On
QNX the same scene peaked at 23.3 cmH2O with no alarm.

## Monolithic contrast

`scripts/m6_run.sh mono 90 mono_bug` runs the same controller and display in one process (`src/mono_vent.c`) and
injects the same display bug. See the README section "Same bug, two architectures".
