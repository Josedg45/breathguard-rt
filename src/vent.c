/* vent.c - modos de ventilación PCV y VCV. Ver vent.h. */
#include "vent.h"

#include <math.h>
#include <string.h>

static double clamp(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }

static void init_common(vent_t *c, const vent_settings_t *set, double pbl_max, double now_s)
{
    memset(c, 0, sizeof *c);
    c->set = *set;
    c->pbl_max = pbl_max;
    c->kp = 2.0;
    c->ki = 15.0;
    c->kq = 20.0;
    c->kiq = 50.0;
    c->r_in = 3.0;
    c->t0 = now_s;
    c->phase = PHASE_EXP;
    c->breath = UINT32_MAX; /* así el primer ciclo cuenta como respiración nueva */
}

void vent_init_pcv(vent_t *c, const vent_settings_t *set, double pbl_max, double now_s)
{
    init_common(c, set, pbl_max, now_s);
    c->mode = MODE_PCV;
}

void vent_init_vcv(vent_t *c, const vent_settings_t *set, double vt, double p_limit, double pbl_max,
                   double now_s)
{
    init_common(c, set, pbl_max, now_s);
    c->mode = MODE_VCV;
    c->vt = vt;
    c->p_limit = p_limit;
}

/* Espiración: soplador a la PEEP, válvula espiratoria abierta con la de PEEP a la PEEP.
 * (Se probó un flujo de base: con solo sensores en la Y no mejora la detección de desconexión con PEEP 0,
 * porque el circuito queda a presión atmosférica. Haría falta un sensor de flujo en la rama espiratoria.) */
static void expiration(vent_t *c, actuators_t *out)
{
    c->target = c->mode == MODE_PCV ? c->set.peep : 0.0;
    out->blower = c->set.peep / c->pbl_max;
    out->exh_open = 1.0;
    out->peep_cmd = c->set.peep;
}

/* PI con antisaturación: el integral solo crece si el soplador no está ya en un tope. hi es el tope superior de
 * presión del soplador (cmH2O). */
static double pi_blower(vent_t *c, double ff, double err, double kp, double ki, double dt, double hi)
{
    double pbl = ff + kp * err + c->integ;
    if ((pbl < hi || err < 0) && (pbl > 0 || err > 0))
        c->integ += ki * err * clamp(dt, 0.0, 0.01);
    return clamp(ff + kp * err + c->integ, 0.0, hi);
}

void vent_step(vent_t *c, double now_s, double dt, const sensors_t *s, actuators_t *out)
{
    const vent_settings_t *v = &c->set;
    const double period = 60.0 / v->rr;
    const double ti = period / (1.0 + v->ie);

    double since = now_s - c->t0;
    uint32_t breath = (uint32_t)floor(since / period);
    double tb = since - (double)breath * period; /* tiempo dentro de la respiración */

    if (breath != c->breath) {
        /* Respiración nueva: el integral no arrastra lo de la anterior. */
        c->integ = 0.0;
        c->limited = 0;
    }
    c->breath = breath;

    if (tb >= ti || c->limited) {
        c->phase = PHASE_EXP;
        expiration(c, out);
        return;
    }

    out->exh_open = 0.0;
    out->peep_cmd = v->peep;

    if (c->mode == MODE_PCV) {
        c->phase = PHASE_INSP;
        double ramp = v->rise_s > 0 ? clamp(tb / v->rise_s, 0.0, 1.0) : 1.0;
        c->target = v->peep + (v->p_insp - v->peep) * ramp;
        /* Fuente de presión limitada: el soplador nunca pasa de p_insp + PCV_HEADROOM. Una respiración normal
         * usa 2-3 cmH2O por encima de p_insp; sin este tope, con el tubo desconectado el PI lleva el soplador a
         * 60 cmH2O y al reconectar el paciente recibe 44 cmH2O en milisegundos (medido en la demo de M7). */
        double hi = clamp(v->p_insp + PCV_HEADROOM, 0.0, c->pbl_max);
        out->blower = pi_blower(c, c->target, c->target - s->paw, c->kp, c->ki, dt, hi) / c->pbl_max;
        return;
    }

    /* VCV */
    if (s->paw > c->p_limit) { /* límite de presión: se corta la inspiración */
        c->limited = 1;
        c->limit_count++;
        c->phase = PHASE_EXP;
        expiration(c, out);
        return;
    }
    const double t_flow = ti * VCV_FLOW_FRACTION;
    if (tb < t_flow) {
        c->phase = PHASE_INSP;
        /* Rampa de subida corta; el flujo de meseta compensa el volumen que se pierde en ella. */
        double rise = v->rise_s > 0 ? v->rise_s : 0.0;
        double q_set = c->vt / (t_flow - rise / 2.0);
        c->target = q_set * (rise > 0 ? clamp(tb / rise, 0.0, 1.0) : 1.0);
        /* Prealimentación: para empujar q por la rama inspiratoria hace falta Paw + q·R_in. */
        double ff = s->paw + c->target * c->r_in;
        out->blower = pi_blower(c, ff, c->target - s->flow, c->kq, c->kiq, dt, c->pbl_max) / c->pbl_max;
    } else {
        c->phase = PHASE_PAUSE; /* pausa inspiratoria: sin flujo, válvula espiratoria cerrada */
        c->target = 0.0;
        out->blower = 0.0;
    }
}
