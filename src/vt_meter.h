/* vt_meter.h - tidal volume meter from the measured flow. No QNX APIs: time is passed in as an argument,
 * so it is tested with synthetic flows (tests/test_vt_meter.c).
 *
 * A breath starts when insp goes from 0 to 1. During it the flow is integrated:
 *   Vti (inspired volume) = volume integrated when inspiration ends (insp 1 -> 0)
 *   Vte (expired volume)  = what came out during expiration, as a positive number
 * If Vte is much smaller than Vti, air is escaping: leak or disconnection.
 *
 * Rules:
 *   1. Integration by the trapezoidal rule, (q_prev + q) / 2 * dt, with the REAL dt (t - t_prev): on the VM
 *      the loop sometimes loses ticks, so 1 ms cannot be assumed.
 *   2. End of inspiration: integrate the interval that just ended first, then latch Vti.
 *   3. Start of a new inspiration: integrate the interval first; if a breath was in progress, Vte = volume at
 *      the end of its inspiration - current volume (positive) and breaths++. Then the volume restarts at 0.
 *   4. Gaps: an interval longer than VTM_MAX_GAP_S is not integrated (what happened in between is unknown)
 *      and is counted in gaps.
 *   5. Time that does not move forward (dt <= 0): not integrated, counted in gaps; the sample still becomes
 *      the previous one.
 *   6. The first breath starts at the first sample with insp = 1; anything before it does not count.
 */
#ifndef VT_METER_H
#define VT_METER_H

#include <stdint.h>

#define VTM_MAX_GAP_S 0.050 /* a longer gap between samples is not integrated */

typedef struct {
    int started;         /* 1 since the first start of inspiration */
    int have_prev;       /* 1 if there is a previous sample */
    int prev_insp;       /* phase of the previous sample */
    double t_prev;       /* time of the previous sample (s) */
    double q_prev;       /* flow of the previous sample (L/s) */
    double vol;          /* volume integrated since the current breath started (L) */
    double vol_end_insp; /* vol at the end of the current breath's inspiration (L) */
    double vti, vte;     /* last complete Vti and Vte (L); 0 until there is one */
    uint32_t breaths;    /* breaths with a complete Vte */
    uint32_t gaps;       /* intervals not integrated because of a gap or time not moving forward */
} vt_meter_t;

void vtm_init(vt_meter_t *m);

/* One sample: time (s), flow towards the patient (L/s, negative while exhaling) and phase
 * (insp = 1 in inspiration, pause included; 0 in expiration). */
void vtm_update(vt_meter_t *m, double t_s, double flow, int insp);

#endif
