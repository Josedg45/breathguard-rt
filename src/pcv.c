/* pcv.c - control por presión. Ver pcv.h. */
#include "pcv.h"

#include <math.h>
#include <string.h>

static double clamp(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }

void pcv_init(pcv_t *c, const vent_settings_t *set, double pbl_max, double now_s)
{
    memset(c, 0, sizeof *c);
    c->set = *set;
    c->pbl_max = pbl_max;
    c->kp = 2.0;
    c->ki = 15.0;
    c->t0 = now_s;
    c->phase = PHASE_EXP;
}

void pcv_step(pcv_t *c, double now_s, double dt, const sensors_t *s, actuators_t *out)
{
    const vent_settings_t *v = &c->set;
    const double period = 60.0 / v->rr;
    const double ti = period / (1.0 + v->ie);

    double since = now_s - c->t0;
    uint32_t breath = (uint32_t)floor(since / period);
    double tb = since - (double)breath * period; /* tiempo dentro de la respiración */
    int phase = tb < ti ? PHASE_INSP : PHASE_EXP;

    if (phase == PHASE_INSP && (c->phase != PHASE_INSP || breath != c->breath))
        c->integ = 0.0; /* cada inspiración empieza sin arrastrar el integral de la anterior */
    c->phase = phase;
    c->breath = breath;

    if (phase == PHASE_INSP) {
        double ramp = v->rise_s > 0 ? clamp(tb / v->rise_s, 0.0, 1.0) : 1.0;
        c->target = v->peep + (v->p_insp - v->peep) * ramp;
        double err = c->target - s->paw;

        /* PI con prealimentación (el objetivo mismo) y antisaturación: el integral solo crece si
         * el soplador no está ya en un tope. */
        double pbl = c->target + c->kp * err + c->integ;
        if ((pbl < c->pbl_max || err < 0) && (pbl > 0 || err > 0))
            c->integ += c->ki * err * clamp(dt, 0.0, 0.01);
        pbl = clamp(c->target + c->kp * err + c->integ, 0.0, c->pbl_max);

        out->blower = pbl / c->pbl_max;
        out->exh_open = 0.0;
        out->peep_cmd = v->peep;
    } else {
        c->target = v->peep;
        out->blower = v->peep / c->pbl_max;
        out->exh_open = 1.0;
        out->peep_cmd = v->peep;
    }
}
