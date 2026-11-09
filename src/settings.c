/* settings.c - M6: validacion y calendario de los ajustes del operador. Ver settings.h. */
#include "settings.h"

#include <math.h>
#include <stdio.h>

static int reject(char *why, size_t n, const char *msg, double v)
{
    snprintf(why, n, msg, v);
    return 0;
}

int settings_validate(const bg_settings_t *cur, const bg_settings_t *req, char *why, size_t n)
{
    const vent_settings_t *s = &req->set;
    /* Un NaN pasa por todas las comparaciones como "falso": hay que descartarlo explicitamente. */
    if (!isfinite(s->rr) || !isfinite(s->ie) || !isfinite(s->p_insp) || !isfinite(s->peep) || !isfinite(req->vt))
        return reject(why, n, "valor no numerico%.0s", 0);
    if (s->rr < SET_RR_MIN || s->rr > SET_RR_MAX) return reject(why, n, "rr %.1f fuera de 6-30 rpm", s->rr);
    if (s->ie < SET_IE_MIN || s->ie > SET_IE_MAX) return reject(why, n, "I:E 1:%.1f fuera de 1:1-1:4", s->ie);
    if (s->peep < SET_PEEP_MIN || s->peep > SET_PEEP_MAX)
        return reject(why, n, "PEEP %.1f fuera de 0-15 cmH2O", s->peep);
    if (req->vcv) {
        if (req->vt < SET_VT_MIN || req->vt > SET_VT_MAX)
            return reject(why, n, "Vt %.0f mL fuera de 200-800", req->vt * 1e3);
    } else {
        if (s->p_insp < SET_P_INSP_MIN || s->p_insp > SET_P_INSP_MAX)
            return reject(why, n, "p_insp %.1f fuera de 5-35 cmH2O", s->p_insp);
        if (s->p_insp - s->peep < SET_DRIVE_MIN)
            return reject(why, n, "p_insp - PEEP = %.1f < 5 cmH2O", s->p_insp - s->peep);
    }
    if (!settings_rate_ok(cur, req, why, n)) return 0;
    snprintf(why, n, "ok");
    return 1;
}

double settings_next_breath(const bg_settings_t *cur, double now)
{
    double period = 60.0 / cur->set.rr;
    double k = ceil((now + SET_LEAD_S - cur->t0) / period);
    if (k < 1) k = 1;
    return cur->t0 + k * period;
}

uint32_t settings_breath(const bg_settings_t *cur, double now, int *insp, double *into)
{
    double period = 60.0 / cur->set.rr, ti = period / (1.0 + cur->set.ie);
    double since = now - cur->t0;
    uint32_t b = since >= 0 ? (uint32_t)(since / period) : 0;
    double in = since >= 0 ? since - b * period : 0;
    if (insp) *insp = since >= 0 && in < ti;
    if (into) *into = in;
    return b;
}
