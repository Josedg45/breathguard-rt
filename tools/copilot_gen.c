/* copilot_gen.c - training data for the copilot's decision tree, from the offline test bench (Ubuntu, no QNX).
 *
 * Each scenario is one simulated patient + one set of ventilator settings + one condition, ventilated with the same
 * plant.c / vent.c / breath_feat.c that run in QNX. The label is known because we put the condition there:
 *   0 normal      R 8-25 cmH2O/(L/s), C 40-80 mL/cmH2O, no leak
 *   1 leak        normal R and C + a leak on the patient side (cuff/mask), g 0.0015-0.012 L/s per cmH2O (leaks of ~10-50 % of Vt)
 *   2 high_r      R 40-100 (bronchospasm, secretions or a kinked tube), normal C
 *   3 low_c       C 12-30 (stiff lung), normal R
 * The gaps (R 25-40, C 30-40) are left out on purpose: there the right answer is debatable.
 * Breaths that are not valid for breath_feat (e.g. VCV breaths cut by the 40 cmH2O pressure limit, where the fixed
 * high-pressure alarm already speaks) are not written; the count per class goes to stderr.
 * One CSV row per closed breath (the first BREATHS_SKIP breaths of each scenario are the start-up transient).
 * Build: make build/copilot_gen   Use: build/copilot_gen -n 2000 -s 1 -o data.csv */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "breath_feat.h"
#include "plant.h"
#include "settings.h"
#include "vent.h"

#define BREATHS_SKIP 3
#define BREATHS_KEEP 10

static uint64_t rng = 1;
static double urand(double lo, double hi)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return lo + (hi - lo) * ((double)(rng >> 11) / 9007199254740992.0);
}

static const char *const cls_names[] = { "normal", "leak", "high_r", "low_c" };

int main(int argc, char **argv)
{
    int n = 400;
    const char *out_path = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "n:s:o:")) != -1) {
        switch (opt) {
        case 'n': n = atoi(optarg); break;
        case 's': rng = 0x9E3779B97F4A7C15ULL ^ (uint64_t)atoll(optarg) * 0xD1B54A32D192ED03ULL; break;
        case 'o': out_path = optarg; break;
        default: fprintf(stderr, "usage: %s [-n scenarios] [-s seed] [-o out.csv]\n", argv[0]); return 2;
        }
    }
    FILE *out = out_path ? fopen(out_path, "w") : stdout;
    if (!out) { perror(out_path); return 1; }
    fprintf(out, "scen,cls,cls_name,vcv,peep,p_insp,vt_ml,rr,ie,r_pat,c_ml,g_leak,leak_true,breath");
    for (int k = 0; k < BF_N; k++) fprintf(out, ",%s", bf_names[k]);
    fprintf(out, "\n");

    long rows = 0, invalid[4] = { 0 };
    for (int sc = 0; sc < n; sc++) {
        int cls = sc % 4;
        plant_params_t pp = plant_default_params();
        pp.r_pat = urand(8, 25);
        pp.c_pat = urand(40, 80) / 1e3;
        double g_leak = 0;
        if (cls == 1) g_leak = urand(0.0015, 0.012);
        if (cls == 2) pp.r_pat = urand(40, 100);
        if (cls == 3) pp.c_pat = urand(12, 30) / 1e3;
        /* the circuit and the sensors also vary a little between machines */
        pp.c_tube *= urand(0.7, 1.3);
        pp.r_in *= urand(0.8, 1.2);
        double nz = urand(0.5, 2.0);
        pp.noise_paw *= nz;
        pp.noise_flow *= nz;

        bg_settings_t cur = { 0 };
        cur.vcv = urand(0, 1) < 0.5;
        cur.set.peep = urand(5, 12);
        cur.set.p_insp = fmin(cur.set.peep + urand(10, 20), SET_P_INSP_MAX);
        cur.vt = urand(0.35, 0.60);
        /* clinical Ti of 0.8-1.4 s, with I:E kept within what safety_mon accepts (1:1 to 1:4) */
        do {
            cur.set.rr = urand(10, 24);
            cur.set.ie = 60.0 / cur.set.rr / urand(0.8, 1.4) - 1.0;
        } while (cur.set.ie < SET_IE_MIN || cur.set.ie > SET_IE_MAX);
        cur.set.rise_s = 0.1;
        cur.t0 = 0;

        plant_t pl;
        plant_init(&pl, &pp, cur.set.peep);
        pl.rng ^= (uint64_t)(sc + 1) * 0xA24BAED4963EE407ULL; /* independent sensor noise per scenario */
        pl.f.g_leak = g_leak;
        vent_t v;
        if (cur.vcv) vent_init_vcv(&v, &cur.set, cur.vt, 40.0, pp.pbl_max, 0.0);
        else vent_init_pcv(&v, &cur.set, pp.pbl_max, 0.0);
        breath_feat_t bf;
        bf_init(&bf);

        actuators_t a = { .blower = cur.set.peep / pp.pbl_max, .exh_open = 1, .peep_cmd = cur.set.peep };
        const double dt = 0.001;
        double v_leak = 0, v_in = 0; /* true leak and inspired volume of the breath in progress */
        double leak_true = 0;
        for (long i = 0; bf.breaths < BREATHS_SKIP + BREATHS_KEEP; i++) {
            double t = i * dt;
            sensors_t s;
            plant_sense(&pl, &s);
            vent_step(&v, t, dt, &s, &a);
            int insp;
            settings_breath(&cur, t, &insp, NULL);
            if (bf_update(&bf, t, s.paw, s.flow, insp, cur.set.peep, cur.vcv)) {
                leak_true = v_in > 0 ? v_leak / v_in : 0;
                v_leak = v_in = 0;
                if (bf.breaths > BREATHS_SKIP && !bf.valid) invalid[cls]++;
                if (bf.breaths > BREATHS_SKIP && bf.valid) {
                    fprintf(out, "%d,%d,%s,%d,%.2f,%.2f,%.0f,%.2f,%.2f,%.2f,%.2f,%.4f,%.4f,%u", sc, cls,
                            cls_names[cls], cur.vcv, cur.set.peep, cur.set.p_insp, cur.vt * 1e3, cur.set.rr,
                            cur.set.ie, pp.r_pat, pp.c_pat * 1e3, g_leak, leak_true, bf.breaths);
                    for (int k = 0; k < BF_N; k++) fprintf(out, ",%.6g", bf.f[k]);
                    fprintf(out, "\n");
                    rows++;
                }
            }
            plant_advance(&pl, &a, dt);
            double q_leak = pl.q_y - pl.q_lung;
            v_leak += q_leak * dt;
            if (insp && pl.q_y > 0) v_in += pl.q_y * dt;
        }
    }
    fprintf(stderr, "copilot_gen: %d scenarios, %ld valid breaths; not valid: normal %ld, leak %ld, high_r %ld, "
            "low_c %ld\n", n, rows, invalid[0], invalid[1], invalid[2], invalid[3]);
    if (out != stdout) fclose(out);
    return 0;
}
