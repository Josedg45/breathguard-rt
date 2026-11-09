/* lung_mon.c - auto-PEEP y mecanica por respiracion. Ver lung_mon.h. */
#include "lung_mon.h"

#include <math.h>
#include <string.h>

#define FIT_SKIP_S 0.10  /* se descarta el comienzo de la espiracion (la valvula y la tubuladura se acomodan) */
#define FIT_Q_MIN 0.03   /* L/s: por debajo, el ruido del sensor (0.01) domina el logaritmo */
#define RC_Q_MIN 0.10    /* L/s: R_circ solo con flujo claro */
#define EI_S 0.020       /* ventana del final de la inspiracion */
#define EE_S 0.050       /* ventana del final de la espiracion */

lung_cfg_t lung_default_cfg(void)
{
    lung_cfg_t c = { .q_eef = 0.05, .auto_peep_max = 3.0 };
    return c;
}

void lung_init(lung_mon_t *m, const lung_cfg_t *cfg)
{
    memset(m, 0, sizeof *m);
    m->cfg = *cfg;
    m->t_exp0 = -1;
    m->auto_peep = m->q_ee = m->tau = NAN;
}

/* Promedio de las muestras del anillo con t > t_end - win. */
static void ring_mean(const lung_mon_t *m, double t_end, double win, double *p, double *q)
{
    double sp = 0, sq = 0;
    int n = 0;
    for (unsigned k = 0; k < LM_RING; k++) {
        unsigned i = (m->ri + LM_RING - 1 - k) % LM_RING;
        if (m->ring_t[i] <= t_end - win || m->ring_t[i] == 0) break;
        sp += m->ring_p[i];
        sq += m->ring_q[i];
        n++;
    }
    *p = n ? sp / n : NAN;
    *q = n ? sq / n : NAN;
}

/* Cierra la espiracion que termina en t: calcula tau, R y auto-PEEP. */
static void finish(lung_mon_t *m, double t)
{
    double p_ee, q_ee;
    ring_mean(m, t, EE_S, &p_ee, &q_ee);
    m->q_ee = q_ee;
    m->eef = fabs(q_ee) > m->cfg.q_eef;
    m->valid = 0;
    double n = m->n_fit, den = n * m->sxx - m->sx * m->sx;
    if (n >= 50 && den > 0 && m->s_qq > 0 && isfinite(m->paw_ei)) {
        double slope = (n * m->sxy - m->sx * m->sy) / den;
        double icpt = (m->sy - slope * m->sx) / n;
        double tau = -1.0 / slope, q0 = exp(icpt);       /* |Q0| */
        double r_circ = m->s_pq / m->s_qq;
        double q_ei = m->q_ei > 0 ? m->q_ei : 0;
        double r_pat = (m->paw_ei - m->peep - q0 * r_circ) / (q0 + q_ei);
        if (slope < 0 && tau > 0.05 && tau < 20 && q0 > 0.05 && r_pat > 0) {
            m->tau = tau;
            m->r_circ = r_circ;
            m->r_tot = r_pat + r_circ;
            m->c = tau / m->r_tot;
            m->auto_peep = (q_ee < 0 ? -q_ee : 0) * m->r_tot;
            m->valid = 1;
        }
    }
    m->breaths++;
    if (m->valid && m->auto_peep > m->cfg.auto_peep_max) m->over++;
    else m->over = 0;
    m->alarm = m->over >= 2;
}

int lung_update(lung_mon_t *m, double t, double paw, double flow, int insp, double peep)
{
    int done = 0;
    if (m->have_prev && insp && !m->prev_insp && m->t_exp0 >= 0) { /* empieza otra inspiracion */
        finish(m, t);
        done = 1;
        m->t_exp0 = -1;
    }
    if (m->have_prev && !insp && m->prev_insp) {                   /* termina la inspiracion */
        ring_mean(m, t, EI_S, &m->paw_ei, &m->q_ei);
        m->t_exp0 = t;
        m->peep = peep;
        m->sx = m->sy = m->sxx = m->sxy = m->n_fit = m->s_pq = m->s_qq = 0;
    }
    if (!insp && m->t_exp0 >= 0) {
        double x = t - m->t_exp0;
        if (x > FIT_SKIP_S && flow < -FIT_Q_MIN) {
            double y = log(-flow);
            m->sx += x; m->sy += y; m->sxx += x * x; m->sxy += x * y; m->n_fit += 1;
        }
        if (flow < -RC_Q_MIN) { m->s_pq += (paw - peep) * -flow; m->s_qq += flow * flow; }
    }
    m->ring_p[m->ri] = paw;
    m->ring_q[m->ri] = flow;
    m->ring_t[m->ri] = t;
    m->ri = (m->ri + 1) % LM_RING;
    m->prev_insp = insp;
    m->have_prev = 1;
    return done;
}
