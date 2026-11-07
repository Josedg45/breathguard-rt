/* m3_offline.c - planta + PCV en un solo lazo, en Ubuntu, sin QNX ni tiempo real.
 * Sirve para ajustar el controlador y como referencia: la versión QNX debe dar lo mismo.
 * Compilar: make build/m3_offline
 * Uso: m3_offline segundos pcv|vcv p_insp|vt_ml peep rr salida.csv [r_pat c_pat_ml] */
#include <stdio.h>
#include <stdlib.h>

#include "vent.h"
#include "plant.h"

int main(int argc, char **argv)
{
    if (argc != 7 && argc != 9) {
        fprintf(stderr, "uso: m3_offline segundos pcv|vcv p_insp|vt_ml peep rr salida.csv [r_pat c_pat_ml]\n");
        return 1;
    }
    double secs = atof(argv[1]);
    int vcv = argv[2][0] == 'v';
    vent_settings_t set = { .rr = atof(argv[5]), .ie = 2.0, .p_insp = atof(argv[3]),
                            .peep = atof(argv[4]), .rise_s = 0.1 };
    FILE *out = fopen(argv[6], "w");
    if (!out) { perror(argv[6]); return 1; }

    plant_params_t pp = plant_default_params();
    if (argc == 9) { pp.r_pat = atof(argv[7]); pp.c_pat = atof(argv[8]) / 1e3; }
    plant_t pl;
    plant_init(&pl, &pp, set.peep);
    vent_t c;
    if (vcv) vent_init_vcv(&c, &set, atof(argv[3]) / 1e3, 40.0, pp.pbl_max, 0.0);
    else vent_init_pcv(&c, &set, pp.pbl_max, 0.0);

    const double dt = 0.001;
    actuators_t a = { .blower = set.peep / pp.pbl_max, .exh_open = 1, .peep_cmd = set.peep };
    fprintf(out, "t_s,phase,target,paw_meas,flow_meas,paw,q_y,v_l,pbl,blower,exh_open\n");
    for (long i = 0; i < (long)(secs / dt); i++) {
        sensors_t s;
        plant_sense(&pl, &s);
        vent_step(&c, i * dt, dt, &s, &a);
        fprintf(out, "%.3f,%d,%.3f,%.3f,%.4f,%.3f,%.4f,%.5f,%.3f,%.4f,%.2f\n", i * dt, c.phase,
                c.target, s.paw, s.flow, pl.paw, pl.q_y, pl.v, pl.pbl, a.blower, a.exh_open);
        plant_advance(&pl, &a, dt);
    }
    fclose(out);
    if (vcv) printf("cortes por límite de presión: %u\n", c.limit_count);
    return 0;
}
