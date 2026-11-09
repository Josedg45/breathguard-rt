/* breath_feat.h - features of one breath for the copilot's decision tree. Plain C, no QNX.
 *
 * Fed with the same 1 kHz samples safety_mon already reads (Paw and flow at the Y-piece) and the phase from the
 * settings schedule (settings_breath). When a breath closes (the next inspiration starts), it computes a fixed vector
 * of BF_N numbers. These are deterministic measurements, not learning: the learned part is only the tree that
 * reads them (copilot_tree.h). The very same code runs offline (tools/copilot_gen.c, training data) and in QNX
 * (safety_mon), so the tree sees in QNX exactly what it was trained on. */
#ifndef BREATH_FEAT_H
#define BREATH_FEAT_H

#include <stdint.h>

#define BF_MAX_GAP_S 0.050 /* a larger gap between samples is not integrated (same rule as vt_meter) */
#define BF_EI_S 0.020      /* window at the end of inspiration */
#define BF_EE_S 0.050      /* window at the end of expiration */

enum {
    BF_VCV = 0,   /* 0 PCV, 1 VCV */
    BF_LEAK,      /* (Vti - Vte) / Vti: fraction of the inspired volume that did not come back */
    BF_CDYN,      /* Vti / (P_ei - PEEP), mL/cmH2O: ~ compliance (P_ei = Paw at the end of inspiration) */
    BF_PRES,      /* PIP - P_ei, cmH2O: resistive pressure (large in VCV with high resistance) */
    BF_TAU_EXP,   /* Vte / peak expiratory flow, s: ~ R*C for an exponential expiration */
    BF_Q_EE,      /* flow at the end of expiration, L/s: < 0 still exhaling (auto-PEEP), > 0 leak at PEEP */
    BF_QEI_REL,   /* flow at the end of inspiration / peak inspiratory flow: PCV with a slow lung stays high */
    BF_G_INSP,    /* peak inspiratory flow / (PIP - PEEP), L/s per cmH2O: ~ 1/R in PCV */
    BF_N
};

extern const char *const bf_names[BF_N];

typedef struct {
    /* running accumulators of the breath in progress */
    int have_prev, prev_insp, in_breath, had_exp;
    double t_prev, q_prev, t_ins0, t_exp0;
    double vin, vout;           /* L inspired (flow > 0 during insp) and expired (flow < 0 during exp) */
    double pip, q_ipk, q_epk;   /* cmH2O, L/s, L/s (expiratory peak as a positive number) */
    double s_pei, s_qei; int n_ei;  /* end-of-inspiration window (last BF_EI_S of the inspiration) */
    double s_qee; int n_ee;         /* end-of-expiration window */
    double ring_t[64], ring_p[64], ring_q[64]; unsigned ri;
    double peep;
    int vcv;
    /* result of the last closed breath */
    double f[BF_N];
    double vti, vte;            /* L */
    uint32_t breaths;           /* closed breaths */
    int valid;                  /* 1 if f[] is meaningful (enough volume and pressure) */
} breath_feat_t;

void bf_init(breath_feat_t *b);

/* One sample. insp = 1 during inspiration (pause included). peep and vcv are the settings in force.
 * Returns 1 when a breath has just closed (f[] updated; check valid). */
int bf_update(breath_feat_t *b, double t, double paw, double flow, int insp, double peep, int vcv);

#endif
