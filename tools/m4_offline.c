/* m4_offline.c - planta + ventilador + alarmas en Ubuntu, sin QNX: prueba la lógica de alarmas.
 *   1. Falsas alarmas: 120 s de ventilación normal con varios ajustes y pulmones.
 *   2. Latencia de detección de desconexión según el momento de la respiración.
 *   3. Presión alta con un controlador que NO limita la presión (falla del control).
 * Compilar: make build/m4_offline */
#include <stdio.h>
#include <stdlib.h>

#include "alarms.h"
#include "plant.h"
#include "vent.h"

typedef struct {
    const char *name;
    int vcv;
    double p_or_vt, peep, rr, r, c;
} scenario_t;

/* Simula hasta t_end; inyecta 'fault' en t_fault (si >= 0). Devuelve las alarmas levantadas y, en
 * *lat, la latencia de la primera alarma de tipo 'watch' tras la falla. */
static uint32_t run(const scenario_t *s, double t_end, double t_fault, int fault, double p_limit, uint32_t watch,
                    double *lat)
{
    plant_params_t pp = plant_default_params();
    pp.r_pat = s->r;
    pp.c_pat = s->c / 1e3;
    plant_t pl;
    plant_init(&pl, &pp, s->peep);
    vent_settings_t set = { .rr = s->rr, .ie = 2.0, .p_insp = s->p_or_vt, .peep = s->peep, .rise_s = 0.1 };
    vent_t v;
    if (s->vcv) vent_init_vcv(&v, &set, s->p_or_vt / 1e3, p_limit, pp.pbl_max, 0.0);
    else vent_init_pcv(&v, &set, pp.pbl_max, 0.0);
    alarm_cfg_t cfg = alarms_default_cfg(s->peep);
    alarms_t al;
    alarms_init(&al, &cfg, 0.0);

    actuators_t a = { .blower = s->peep / pp.pbl_max, .exh_open = 1, .peep_cmd = s->peep };
    uint32_t all = 0;
    *lat = -1;
    const double dt = 0.001;
    int injected = 0;
    double t_ref = -1; /* referencia de la latencia: la falla, o el cruce del umbral de presión */
    for (long i = 0; i * dt < t_end; i++) {
        double t = i * dt;
        if (t_fault >= 0 && t >= t_fault && !injected) {
            injected = 1;
            if (fault == 1) { pl.f.disconnected = 1; t_ref = t; }
            if (fault == 2) pl.f.obstruction = 8.0;
        }
        if (fault == 2 && injected && t_ref < 0 && pl.paw > cfg.p_high) t_ref = t;
        sensors_t sn;
        plant_sense(&pl, &sn);
        vent_step(&v, t, dt, &sn, &a);
        uint32_t up = alarms_update(&al, t, sn.paw, sn.flow);
        all |= up;
        if ((up & watch) && *lat < 0 && t_ref >= 0) *lat = t - t_ref;
        plant_advance(&pl, &a, dt);
    }
    return all;
}

static void print_alarms(uint32_t m)
{
    if (!m) { printf("ninguna"); return; }
    for (int b = 0; b < ALARM_COUNT; b++)
        if (m & (1u << b)) printf("%s ", alarm_name(1u << b));
}

int main(void)
{
    const scenario_t sc[] = {
        { "PCV 20/5 12rpm normal", 0, 20, 5, 12, 22.4, 58 },
        { "PCV 30/10 20rpm normal", 0, 30, 10, 20, 22.4, 58 },
        { "PCV 12/0 10rpm normal", 0, 12, 0, 10, 22.4, 58 },
        { "PCV 25/5 rígido C=20", 0, 25, 5, 16, 10, 20 },
        { "VCV 500/5 normal", 1, 500, 5, 12, 22.4, 58 },
        { "VCV 400/5 obstruido R=40", 1, 400, 5, 12, 40, 58 },
        { "VCV 450/0 normal", 1, 450, 0, 14, 22.4, 58 },
    };
    const int n = sizeof sc / sizeof sc[0];
    int bad = 0;
    double lat;

    puts("1. Falsas alarmas en 120 s de ventilación normal");
    for (int i = 0; i < n; i++) {
        uint32_t m = run(&sc[i], 120, -1, 0, 40, 0, &lat);
        printf("   %-26s ", sc[i].name);
        print_alarms(m);
        puts("");
        if (m) bad++;
    }

    puts("\n2. Desconexión: latencia según el momento de la respiración");
    for (int i = 0; i < n; i++) {
        double period = 60.0 / sc[i].rr, worst = 0, best = 1e9;
        int missed = 0;
        for (int k = 0; k < 20; k++) {
            double tf = 3 * period + period * k / 20.0; /* 20 puntos a lo largo de una respiración */
            run(&sc[i], tf + 15, tf, 1, 40, ALARM_DISCONNECT, &lat);
            if (lat < 0) { missed++; continue; }
            if (lat > worst) worst = lat;
            if (lat < best) best = lat;
        }
        printf("   %-26s mín %5.0f ms, máx %5.0f ms, no detectadas %d de 20\n", sc[i].name, best * 1e3,
               worst * 1e3, missed);
        if (missed) bad++;
    }

    puts("\n3. Obstrucción súbita (R x8) en VCV con un controlador que NO limita la presión");
    puts("   (latencia desde que la Paw real cruza 40 cmH2O)");
    for (int i = 4; i < n; i++) {
        double period = 60.0 / sc[i].rr;
        uint32_t m = run(&sc[i], 3 * period + 6, 3 * period, 2, 1e9, ALARM_HIGH_PRESSURE, &lat);
        printf("   %-26s alarmas: ", sc[i].name);
        print_alarms(m);
        printf(" latencia %s%.0f ms\n", lat < 0 ? "(no) " : "", lat * 1e3);
    }

    printf("\nRESULTADO: %s\n", bad ? "HAY PROBLEMAS" : "sin falsas alarmas y todas las desconexiones detectadas");
    return bad != 0;
}
