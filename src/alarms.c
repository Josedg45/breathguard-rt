/* alarms.c - detección de alarmas por sensores. Ver alarms.h. */
#include "alarms.h"

#include <string.h>

alarm_cfg_t alarms_default_cfg(double peep)
{
    alarm_cfg_t c = {
        .p_alarm = 38.0, .p_high = 40.0, .t_high = 0.003,
        .p_low = peep > 4.0 ? peep - 3.0 : 1.5,
        .q_disc_low = 0.3, .q_disc_high = 2.0, .t_disc = 0.020,
        .p_breath = peep + 5.0, .apnea_s = 12.0,
    };
    return c;
}

void alarms_init(alarms_t *a, const alarm_cfg_t *cfg, double now_s)
{
    memset(a, 0, sizeof *a);
    a->cfg = *cfg;
    a->alarm_since = a->high_since = a->disc_since = -1.0;
    a->last_breath = now_s;
}

/* Condición sostenida: devuelve 1 si 'cond' se cumple desde hace al menos 'hold' segundos. */
static int sustained(double *since, int cond, double t, double hold)
{
    if (!cond) { *since = -1.0; return 0; }
    if (*since < 0) *since = t;
    return t - *since >= hold;
}

uint32_t alarms_update(alarms_t *a, double t, double paw, double flow)
{
    const alarm_cfg_t *c = &a->cfg;
    uint32_t now = a->active & ~(ALARM_HIGH_PRESSURE | ALARM_DISCONNECT | ALARM_APNEA);

    if (sustained(&a->alarm_since, paw > c->p_alarm, t, c->t_high)) now |= ALARM_HIGH_PRESSURE;
    a->relief = sustained(&a->high_since, paw > c->p_high, t, c->t_high);

    int disc = (paw < c->p_low && flow > c->q_disc_low) || flow > c->q_disc_high;
    if (sustained(&a->disc_since, disc, t, c->t_disc)) now |= ALARM_DISCONNECT;

    int above = paw > c->p_breath;
    if (above && !a->above_breath) a->last_breath = t;
    a->above_breath = above;
    if (t - a->last_breath > c->apnea_s) now |= ALARM_APNEA;

    uint32_t raised = now & ~a->active;
    a->active = now;
    return raised;
}

int alarms_relief(const alarms_t *a) { return a->relief; }

const char *alarm_name(uint32_t bit)
{
    switch (bit) {
    case ALARM_HIGH_PRESSURE: return "PRESION_ALTA";
    case ALARM_DISCONNECT: return "DESCONEXION";
    case ALARM_APNEA: return "APNEA";
    case ALARM_CTRL_DEAD: return "CONTROL_MUERTO";
    case ALARM_CTRL_HANG: return "CONTROL_COLGADO";
    case ALARM_LOW_VT: return "VT_BAJO";
    case ALARM_HIGH_VT: return "VT_ALTO";
    case ALARM_LEAK: return "FUGA";
    case ALARM_POWER: return "ENERGIA";
    case ALARM_AUTO_PEEP: return "AUTO_PEEP";
    case ALARM_SECURITY: return "SEGURIDAD";
    default: return "?";
    }
}
