/* vt_meter.c - tidal volume meter: integrates the measured flow (trapezoidal rule, real dt) into the
 * inspired (Vti) and expired (Vte) volume of each breath. See vt_meter.h. */
#include "vt_meter.h"

#include <string.h>

void vtm_init(vt_meter_t *m)
{
    memset(m, 0, sizeof *m);
}

void vtm_update(vt_meter_t *m, double t_s, double flow, int insp)
{
    /* 1. Integrate the interval since the previous sample with its real length. Gaps longer than
     *    VTM_MAX_GAP_S and intervals that do not move forward are skipped and counted. */
    if (m->have_prev) {
        double dt = t_s - m->t_prev;
        if (dt <= 0.0 || dt > VTM_MAX_GAP_S)
            m->gaps++;
        else if (m->started)
            m->vol += 0.5 * (m->q_prev + flow) * dt;
    }

    /* 2. Phase transitions, after the interval that ends at this sample has been integrated. */
    if (m->started && m->prev_insp && !insp) {
        /* End of inspiration: Vti is the volume delivered since the breath started. */
        m->vti = m->vol;
        m->vol_end_insp = m->vol;
    } else if (insp && (!m->started || !m->prev_insp)) {
        /* Start of a new inspiration (or the first inspiratory sample): close the previous breath. */
        if (m->started) {
            double out = m->vol_end_insp - m->vol;
            m->vte = out > 0.0 ? out : -out;
            m->breaths++;
        }
        m->started = 1;
        m->vol = 0.0;
        m->vol_end_insp = 0.0;
    }

    /* 3. This sample becomes the previous one, valid interval or not. */
    m->have_prev = 1;
    m->prev_insp = insp;
    m->t_prev = t_s;
    m->q_prev = flow;
}
