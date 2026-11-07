/* m5_offline.c - prueba del modelo de batería y del estimador de autonomía, en Ubuntu.
 * 1. Potencia media del ventilador en PCV 20/5 (planta + control) con y sin recorte de consumos.
 * 2. Descarga completa desde el 100 %: autonomía real (hasta 21 V) contra la estimada al cortarse la red.
 * Compilar: make build/m5_offline */
#include <stdio.h>

#include "battery.h"
#include "plant.h"
#include "vent.h"

#define V_CUTOFF 21.0 /* 3.0 V por celda */

int main(void)
{
    /* 1. Potencia media en 5 respiraciones. */
    plant_params_t pp = plant_default_params();
    plant_t pl;
    plant_init(&pl, &pp, 5);
    vent_settings_t set = { .rr = 12, .ie = 2.0, .p_insp = 20, .peep = 5, .rise_s = 0.1 };
    vent_t v;
    vent_init_pcv(&v, &set, pp.pbl_max, 0);
    actuators_t a = { .blower = 5.0 / 60, .exh_open = 1, .peep_cmd = 5 };
    loads_t ld = loads_default();
    double e_full = 0, e_shed = 0, pmax = 0;
    const double dt = 0.001, T = 30;
    for (long i = 0; i * dt < T; i++) {
        sensors_t s;
        plant_sense(&pl, &s);
        vent_step(&v, i * dt, dt, &s, &a);
        plant_advance(&pl, &a, dt);
        double pf = loads_power(&ld, pl.paw, pl.q_y, 0), ps = loads_power(&ld, pl.paw, pl.q_y, 1);
        e_full += pf * dt;
        e_shed += ps * dt;
        if (ps > pmax) pmax = ps;
    }
    double p_full = e_full / T, p_shed = e_shed / T;
    printf("1. Potencia media PCV 20/5: %.1f W con todo, %.1f W recortando (pico %.1f W)\n", p_full, p_shed, pmax);

    /* 2. Descarga a potencia media constante. */
    for (int shed = 0; shed <= 1; shed++) {
        double p = shed ? p_shed : p_full;
        battery_t b;
        { battery_params_t bp = battery_default_params(); battery_init(&b, &bp, 1.0); }
        double est_h = battery_energy_wh(&b, 0.0) / p;
        double t = 0;
        while (b.v > V_CUTOFF && b.soc > 0) {
            battery_step(&b, p, 0, 1.0);
            t += 1.0;
        }
        printf("2. %s: autonomía simulada %.2f h (corte a %.1f V, SoC %.1f %%), estimada al empezar %.2f h "
               "(%+.1f %%)\n", shed ? "recortando" : "con todo  ", t / 3600, b.v, b.soc * 100, est_h,
               (est_h - t / 3600) / (t / 3600) * 100);
    }
    /* 3. Recuperación: tras un pico de corriente la tensión vuelve (rama RC). */
    battery_t b;
    { battery_params_t bp = battery_default_params(); battery_init(&b, &bp, 0.5); }
    for (int k = 0; k < 60; k++) battery_step(&b, 150, 0, 1.0);
    double v_load = b.v;
    for (int k = 0; k < 600; k++) battery_step(&b, 0.001, 0, 1.0);
    printf("3. SoC 50 %%: Voc %.2f V; con 150 W durante 60 s cae a %.2f V; 10 min en reposo: %.2f V\n",
           battery_ocv(0.5), v_load, b.v);
    return 0;
}
