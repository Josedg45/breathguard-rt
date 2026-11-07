/* m3_offline.c - planta + PCV en un solo lazo, en Ubuntu, sin QNX ni tiempo real.
 * Sirve para ajustar el controlador y como referencia: la versión QNX debe dar lo mismo.
 * Compilar: gcc -O2 -Isrc tools/m3_offline.c src/plant.c src/pcv.c -lm -o build/m3_offline
 * Uso: m3_offline segundos p_insp peep rr salida.csv */
#include <stdio.h>
#include <stdlib.h>

#include "pcv.h"
#include "plant.h"

int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "uso: m3_offline segundos p_insp peep rr salida.csv\n");
        return 1;
    }
    double secs = atof(argv[1]);
    vent_settings_t set = { .rr = atof(argv[4]), .ie = 2.0, .p_insp = atof(argv[2]),
                            .peep = atof(argv[3]), .rise_s = 0.1 };
    FILE *out = fopen(argv[5], "w");
    if (!out) { perror(argv[5]); return 1; }

    plant_params_t pp = plant_default_params();
    plant_t pl;
    plant_init(&pl, &pp, set.peep);
    pcv_t c;
    pcv_init(&c, &set, pp.pbl_max, 0.0);

    const double dt = 0.001;
    actuators_t a = { .blower = set.peep / pp.pbl_max, .exh_open = 1, .peep_cmd = set.peep };
    fprintf(out, "t_s,phase,target,paw_meas,flow_meas,paw,q_y,v_l,pbl,blower,exh_open\n");
    for (long i = 0; i < (long)(secs / dt); i++) {
        sensors_t s;
        plant_sense(&pl, &s);
        pcv_step(&c, i * dt, dt, &s, &a);
        fprintf(out, "%.3f,%d,%.3f,%.3f,%.4f,%.3f,%.4f,%.5f,%.3f,%.4f,%.2f\n", i * dt, c.phase,
                c.target, s.paw, s.flow, pl.paw, pl.q_y, pl.v, pl.pbl, a.blower, a.exh_open);
        plant_advance(&pl, &a, dt);
    }
    fclose(out);
    return 0;
}
