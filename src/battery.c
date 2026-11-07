/* battery.c - batería de Thevenin y consumos. Ver battery.h. */
#include "battery.h"

#include <math.h>
#include <string.h>

battery_params_t battery_default_params(void)
{
    /* 7S2P de celdas 18650 de 3 Ah: 6 Ah, ~24 V. R0 y RC típicos de un paquete así. */
    battery_params_t p = { .q_ah = 6.0, .r0 = 0.12, .r1 = 0.08, .c1 = 600.0, .i_charge = 1.5 };
    return p;
}

void battery_init(battery_t *b, const battery_params_t *p, double soc)
{
    memset(b, 0, sizeof *b);
    b->p = *p;
    b->soc = soc;
    b->v = battery_ocv(soc);
}

double battery_ocv(double soc)
{
    /* Curva por tramos de una celda de ion-litio (V) contra SoC, por 7 celdas. */
    static const double s[] = { 0.00, 0.05, 0.10, 0.20, 0.40, 0.60, 0.80, 0.95, 1.00 };
    static const double v[] = { 3.00, 3.30, 3.45, 3.58, 3.70, 3.80, 3.95, 4.10, 4.20 };
    if (soc <= 0) return 7 * v[0];
    if (soc >= 1) return 7 * v[8];
    int k = 0;
    while (soc > s[k + 1]) k++;
    double f = (soc - s[k]) / (s[k + 1] - s[k]);
    return 7 * (v[k] + f * (v[k + 1] - v[k]));
}

void battery_step(battery_t *b, double p_load_w, int mains, double dt)
{
    const battery_params_t *p = &b->p;
    double ocv = battery_ocv(b->soc);
    double i;
    if (mains) {
        i = b->soc < 1.0 ? -p->i_charge : 0.0; /* carga a corriente constante */
    } else {
        /* P = V·I con V = ocv - V1 - I·R0  ->  R0·I² - (ocv - V1)·I + P = 0; raíz pequeña. */
        double e = ocv - b->v1;
        double disc = e * e - 4 * p->r0 * p_load_w;
        i = disc > 0 ? (e - sqrt(disc)) / (2 * p->r0) : e / (2 * p->r0);
    }
    b->i = i;
    b->v1 += (-b->v1 / (p->r1 * p->c1) + i / p->c1) * dt;
    b->soc -= i * dt / (3600.0 * p->q_ah);
    if (b->soc < 0) b->soc = 0;
    if (b->soc > 1) b->soc = 1;
    b->v = ocv - i * p->r0 - b->v1;
}

double battery_energy_wh(const battery_t *b, double soc_min)
{
    /* Integral de Voc·dQ desde soc_min hasta el SoC actual (regla del trapecio, pasos de 1 %). */
    double e = 0;
    for (double s = soc_min; s < b->soc; s += 0.01) {
        double s2 = s + 0.01 < b->soc ? s + 0.01 : b->soc;
        e += 0.5 * (battery_ocv(s) + battery_ocv(s2)) * (s2 - s) * b->p.q_ah;
    }
    return e;
}

loads_t loads_default(void)
{
    loads_t l = { .base = 6.0, .blower_idle = 8.0, .blower_eff = 0.25, .display = 7.0, .display_dim = 2.0,
                  .humidifier = 35.0, .telemetry = 2.0 };
    return l;
}

double loads_power(const loads_t *l, double paw, double flow, int shed)
{
    /* Potencia neumática: Δp (Pa) · Q (m³/s). 1 cmH2O = 98.07 Pa; 1 L/s = 1e-3 m³/s. Solo cuenta el
     * flujo que el soplador entrega (inspiración). */
    double pneu = flow > 0 && paw > 0 ? paw * 98.07 * flow * 1e-3 : 0.0;
    double p = l->base + l->blower_idle + pneu / l->blower_eff;
    /* La telemetría se mantiene siempre: lleva las alarmas al puesto de enfermería. En batería se apaga el
     * humidificador y se atenúa la pantalla. */
    p += l->telemetry + (shed ? l->display_dim : l->display + l->humidifier);
    return p;
}
