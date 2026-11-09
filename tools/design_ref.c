/* design_ref.c - datos de referencia para un modelo independiente (Simulink): la misma planta (plant.c) y el
 * mismo control (vent.c) que corren en QNX, pero en el host con un reloj ideal de 1 ms exacto.
 *
 *   design_ref pcv  salida.csv   PCV 20/5, 12 rpm, I:E 1:2, 60 s, RC fijo
 *   design_ref step salida.csv   escalones en lazo abierto (solo la planta), 16 s; ver step_schedule()
 *
 * Columnas (las mismas que scripts/design_export.py escribe para las corridas de QNX):
 *   t_s, phase (0 insp, 1 esp, 2 pausa; -1 en los escalones), setpoint, paw_meas, flow_meas,
 *   blower_cmd, exh_cmd, peep_cmd, paw_true, flow_true, volume_ml, pbl, palv
 */
#include <stdio.h>
#include <string.h>

#include "plant.h"
#include "vent.h"

#define PEEP0 5.0

/* Escalones en lazo abierto: A) valvula espiratoria cerrada, soplador de 5 a 20 cmH2O en t = 1 s: el pulmon se
 * llena por R_in + R_pat (tau ~1.5 s). B) en t = 10 s el soplador vuelve a 5 y se abre la valvula espiratoria:
 * espiracion pasiva hacia la PEEP por R_pat + R_ex. Lo comparte step_test.c (QNX). */
static void step_schedule(double t, double pbl_max, actuators_t *a)
{
    a->peep_cmd = PEEP0;
    a->blower = (t >= 1.0 && t < 10.0 ? 20.0 : PEEP0) / pbl_max;
    a->exh_open = t >= 10.0 ? 1.0 : 0.0;
}

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[1], "pcv") && strcmp(argv[1], "step"))) {
        fprintf(stderr, "uso: %s pcv|step salida.csv\n", argv[0]);
        return 1;
    }
    int step = !strcmp(argv[1], "step");
    FILE *f = fopen(argv[2], "w");
    if (!f) { perror(argv[2]); return 1; }

    plant_params_t pp = plant_default_params();
    plant_t pl;
    plant_init(&pl, &pp, PEEP0);
    vent_settings_t set = { .rr = 12, .ie = 2.0, .p_insp = 20, .peep = PEEP0, .rise_s = 0.1 };
    vent_t v;
    vent_init_pcv(&v, &set, pp.pbl_max, 0.0);
    actuators_t a = { .blower = PEEP0 / pp.pbl_max, .exh_open = 1, .peep_cmd = PEEP0 };
    if (step) a.exh_open = 0; /* el escalon empieza con la valvula espiratoria cerrada, en reposo a 5 */
    const double v_rest = pp.v0 + pp.c_pat * PEEP0;

    fprintf(f, "t_s,phase,setpoint,paw_meas,flow_meas,blower_cmd,exh_cmd,peep_cmd,paw_true,flow_true,volume_ml,"
               "pbl,palv\n");
    int n = step ? 16000 : 60000;
    for (int k = 0; k < n; k++) {
        double t = k * 1e-3;
        sensors_t s;
        plant_sense(&pl, &s);
        double sp = 0;
        int phase = -1;
        if (step) {
            step_schedule(t, pp.pbl_max, &a);
        } else {
            vent_step(&v, t, 1e-3, &s, &a);
            sp = v.target;
            phase = v.phase;
        }
        /* paw_true/flow_true: el estado en el instante de la lectura (antes de aplicar el comando) */
        fprintf(f, "%.3f,%d,%.4f,%.4f,%.5f,%.5f,%.2f,%.2f,%.4f,%.5f,%.3f,%.4f,%.4f\n", t, phase, sp, s.paw, s.flow,
                a.blower, a.exh_open, a.peep_cmd, pl.paw, pl.q_y, (pl.v - v_rest) * 1e3, pl.pbl, plant_palv(&pl));
        plant_advance(&pl, &a, 1e-3);
    }
    fclose(f);
    printf("design_ref: %s, %d muestras -> %s\n", argv[1], n, argv[2]);
    return 0;
}
