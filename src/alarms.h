/* alarms.h - detección de alarmas a partir de los sensores (safety_mon). C puro, sin QNX.
 *
 * safety_mon lee los sensores por su cuenta (no confía en lo que diga breath_ctrl) y evalúa:
 *   ALARM_HIGH_PRESSURE  Paw > p_alarm durante t_high (un poco por debajo del límite del controlador, para
 *                        que avise aunque el controlador corte la inspiración al llegar al límite)
 *   alivio de presión    Paw > p_high durante t_high: el controlador no limitó -> safety_mon abre la
 *                        válvula espiratoria (alarms_relief())
 *   ALARM_DISCONNECT     flujo saliente por la Y con Paw baja, o flujo enorme, durante t_disc
 *   ALARM_APNEA          ninguna respiración detectada (Paw no sube) en apnea_s
 * Las alarmas de proceso (controlador muerto o colgado) las pone safety_mon directamente. */
#ifndef ALARMS_H
#define ALARMS_H

#include <stdint.h>

enum {
    ALARM_HIGH_PRESSURE = 1u << 0,
    ALARM_DISCONNECT = 1u << 1,
    ALARM_APNEA = 1u << 2,
    ALARM_CTRL_DEAD = 1u << 3,  /* breath_ctrl murió (aviso del kernel) */
    ALARM_CTRL_HANG = 1u << 4,  /* breath_ctrl no late (watchdog) */
    ALARM_LOW_VT = 1u << 5,
    ALARM_HIGH_VT = 1u << 6,
    ALARM_LEAK = 1u << 7,
    ALARM_POWER = 1u << 8,
    ALARM_AUTO_PEEP = 1u << 9,  /* auto-PEEP estimado sobre el limite (lung_mon) */
    ALARM_SECURITY = 1u << 10,  /* la planta rechazó un comando no autorizado (tarea 4) */
};
#define ALARM_COUNT 11

typedef struct {
    double p_alarm;     /* cmH2O: alarma de presión alta */
    double p_high;      /* cmH2O: alivio de presión */
    double t_high;      /* s que debe durar para confirmarse */
    double p_low;       /* cmH2O: por debajo, la Paw se considera "baja" */
    double q_disc_low;  /* L/s: flujo saliente por la Y que, con Paw baja, indica desconexión */
    double q_disc_high; /* L/s: flujo que por sí solo indica desconexión (nadie inspira tanto) */
    double t_disc;      /* s */
    double p_breath;    /* cmH2O: Paw por encima de esto cuenta como respiración */
    double apnea_s;     /* s sin respiración */
} alarm_cfg_t;

typedef struct {
    alarm_cfg_t cfg;
    double alarm_since, high_since, disc_since; /* desde cuándo se cumple; < 0 = no se cumple */
    int relief;                    /* 1 si hace falta alivio de presión ahora */
    double last_breath;            /* última vez que Paw cruzó p_breath hacia arriba */
    int above_breath;
    uint32_t active;               /* alarmas activas ahora */
} alarms_t;

/* Configuración por defecto para una PEEP dada. */
alarm_cfg_t alarms_default_cfg(double peep);

void alarms_init(alarms_t *a, const alarm_cfg_t *cfg, double now_s);

/* Evalúa una muestra. Devuelve las alarmas que se ACTIVARON en esta muestra (flancos). */
uint32_t alarms_update(alarms_t *a, double t_s, double paw, double flow);

/* 1 si la presión supera p_high de forma sostenida: hay que aliviarla (el controlador no lo hizo). */
int alarms_relief(const alarms_t *a);

const char *alarm_name(uint32_t bit);

#endif
