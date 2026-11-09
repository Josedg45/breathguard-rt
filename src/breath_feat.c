/* breath_feat.c - features of one breath. See breath_feat.h. */
#include "breath_feat.h"

#include <math.h>
#include <string.h>

#define RING (sizeof(((breath_feat_t *)0)->ring_t) / sizeof(double))

const char *const bf_names[BF_N] = { "vcv", "leak_frac", "cdyn_ml", "p_res", "tau_exp", "q_ee", "qei_rel",
                                     "g_insp" };

void bf_init(breath_feat_t *b)
{
    memset(b, 0, sizeof *b);
}

/* Mean Paw and flow of the ring samples with t > t_end - win. */
static void ring_mean(const breath_feat_t *b, double t_end, double win, double *p, double *q)
{
    double sp = 0, sq = 0;
    int n = 0;
    for (unsigned k = 0; k < RING; k++) {
        unsigned i = (b->ri + RING - 1 - k) % RING;
        if (b->ring_t[i] <= t_end - win || b->ring_t[i] == 0) break;
        sp += b->ring_p[i];
        sq += b->ring_q[i];
        n++;
    }
    *p = n ? sp / n : NAN;
    *q = n ? sq / n : NAN;
}

static void finish(breath_feat_t *b, double t)
{
    double p_ee, q_ee;
    ring_mean(b, t, BF_EE_S, &p_ee, &q_ee);
    double p_ei = b->n_ei ? b->s_pei : NAN, q_ei = b->n_ei ? b->s_qei : NAN;
    double vti = b->vin, vte = b->vout, drive_ei = p_ei - b->peep, drive_pk = b->pip - b->peep;

    b->vti = vti;
    b->vte = vte;
    b->breaths++;
    b->valid = vti > 0.05 && drive_ei > 2.0 && drive_pk > 2.0 && b->q_epk > 0.05 && b->q_ipk > 0.05 &&
               isfinite(p_ei) && isfinite(q_ee);
    /* f[] is filled even for an invalid breath (useful to see why); the tree only gets valid ones. */
    b->f[BF_VCV] = b->vcv;
    b->f[BF_LEAK] = vti > 0 ? (vti - vte) / vti : NAN;
    b->f[BF_CDYN] = vti * 1e3 / drive_ei;
    b->f[BF_PRES] = b->pip - p_ei;
    b->f[BF_TAU_EXP] = vte > 0 ? vte / b->q_epk : 0.0;
    b->f[BF_Q_EE] = q_ee;
    b->f[BF_QEI_REL] = q_ei / b->q_ipk;
    b->f[BF_G_INSP] = b->q_ipk / drive_pk;
}

int bf_update(breath_feat_t *b, double t, double paw, double flow, int insp, double peep, int vcv)
{
    int done = 0;
    if (b->have_prev) {
        if (insp && !b->prev_insp) { /* a new inspiration: close the previous breath, start this one */
            if (b->in_breath && b->had_exp) { finish(b, t); done = 1; }
            b->in_breath = 1;
            b->had_exp = 0;
            b->t_ins0 = t;
            b->vin = b->vout = 0;
            b->pip = -1e9;
            b->q_ipk = b->q_epk = 0;
            b->n_ei = 0;
            b->peep = peep;
            b->vcv = vcv;
        } else if (!insp && b->prev_insp && b->in_breath) { /* end of inspiration */
            double p, q;
            ring_mean(b, t, BF_EI_S, &p, &q);
            b->s_pei = p;
            b->s_qei = q;
            b->n_ei = isfinite(p);
            b->had_exp = 1;
            b->t_exp0 = t;
        }
        double dt = t - b->t_prev;
        if (b->in_breath && dt > 0 && dt <= BF_MAX_GAP_S) {
            double dv = 0.5 * (flow + b->q_prev) * dt; /* trapezoid */
            if (insp) b->vin += dv;
            else b->vout -= dv;
        }
    }
    if (b->in_breath) {
        if (insp) {
            if (paw > b->pip) b->pip = paw;
            if (flow > b->q_ipk) b->q_ipk = flow;
        } else if (-flow > b->q_epk) {
            b->q_epk = -flow;
        }
    }
    b->ring_t[b->ri] = t;
    b->ring_p[b->ri] = paw;
    b->ring_q[b->ri] = flow;
    b->ri = (b->ri + 1) % RING;
    b->have_prev = 1;
    b->prev_insp = insp;
    b->t_prev = t;
    b->q_prev = flow;
    return done;
}
