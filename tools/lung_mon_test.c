/* lung_mon_test.c - prueba en el host del estimador de auto-PEEP (src/lung_mon.c) contra la planta, que si
 * conoce la presion alveolar verdadera. Planta + PCV/VCV a 1 ms exacto, con ruido de sensores.
 * Uso: build/lung_mon_test   (sale con 1 si algun caso se aleja mas de la tolerancia) */
#include <math.h>
#include <stdio.h>

#include "lung_mon.h"
#include "plant.h"
#include "settings.h"
#include "vent.h"

typedef struct {
    const char *name;
    int vcv;
    double rr, ie, p_insp, peep, vt, r_mult, c_pat;
} case_t;

int main(void)
{
    const case_t cases[] = {
        { "PCV 20/5 12 rpm 1:2 (demo)", 0, 12, 2, 20, 5, 0, 1, 0.058 },
        { "PCV 23/8 12 rpm 1:2", 0, 12, 2, 23, 8, 0, 1, 0.058 },
        { "PCV 20/5 20 rpm 1:1", 0, 20, 1, 20, 5, 0, 1, 0.058 },
        { "PCV 20/5 30 rpm 1:1", 0, 30, 1, 20, 5, 0, 1, 0.058 },
        { "PCV 25/5 20 rpm 1:2, R x2 (obstruccion)", 0, 20, 2, 25, 5, 0, 2, 0.058 },
        { "PCV 15/5 12 rpm 1:2, R 10, C 40 mL", 0, 12, 2, 15, 5, 0, 10.0 / 22.4, 0.040 },
        { "VCV 450 mL 20 rpm 1:1", 1, 20, 1, 0, 5, 0.45, 1, 0.058 },
    };
    int fails = 0;
    printf("%-42s %8s %8s %6s %8s %8s %7s %7s\n", "caso", "real", "estim", "error", "Qee L/m", "tau s", "R est",
           "C mL");
    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        const case_t *c = &cases[k];
        plant_params_t pp = plant_default_params();
        pp.r_pat *= c->r_mult;
        pp.c_pat = c->c_pat;
        plant_t pl;
        plant_init(&pl, &pp, c->peep);
        vent_settings_t set = { .rr = c->rr, .ie = c->ie, .p_insp = c->p_insp, .peep = c->peep, .rise_s = 0.1 };
        vent_t v;
        if (c->vcv) vent_init_vcv(&v, &set, c->vt, 40, pp.pbl_max, 0.5);
        else vent_init_pcv(&v, &set, pp.pbl_max, 0.5);
        bg_settings_t cur = { .vcv = c->vcv, .set = set, .vt = c->vt, .t0 = 0.5 };
        lung_cfg_t cfg = lung_default_cfg();
        lung_mon_t m;
        lung_init(&m, &cfg);
        actuators_t a = { .blower = c->peep / pp.pbl_max, .exh_open = 1, .peep_cmd = c->peep };
        double truth = NAN;
        int prev_insp = 0;
        for (int i = 0; i < 60000; i++) {
            double t = i * 1e-3;
            sensors_t s;
            plant_sense(&pl, &s);
            int insp;
            settings_breath(&cur, t, &insp, NULL);
            if (insp && !prev_insp) truth = plant_palv(&pl) - c->peep; /* Palv al final de la espiracion */
            prev_insp = insp;
            lung_update(&m, t, s.paw, s.flow, insp, c->peep);
            vent_step(&v, t, 1e-3, &s, &a);
            plant_advance(&pl, &a, 1e-3);
        }
        double err = m.auto_peep - truth;
        int bad = !m.valid || fabs(err) > 0.5 + 0.1 * fabs(truth);
        fails += bad;
        printf("%-42s %8.2f %8.2f %6.2f %8.2f %8.3f %7.1f %7.1f %s\n", c->name, truth, m.auto_peep, err,
               m.q_ee * 60, m.tau, m.r_tot, m.c * 1e3, bad ? "FALLA" : "ok");
        printf("%-42s  R real %.1f (pac %.1f + valvula %.1f), C real %.1f mL; R_circ estimada %.2f\n", "",
               pp.r_pat + pp.r_ex, pp.r_pat, pp.r_ex, pp.c_pat * 1e3, m.r_circ);
    }
    printf(fails ? "lung_mon_test: %d casos fuera de tolerancia\n" : "lung_mon_test: todo ok%.0d\n", fails);
    return fails ? 1 : 0;
}
