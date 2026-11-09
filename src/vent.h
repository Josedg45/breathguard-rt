/* vent.h - modos de ventilación de breath_ctrl (PCV y VCV). C puro, sin QNX.
 *
 * PCV (control por presión): en la inspiración, un PI mueve el soplador para que Paw siga una rampa
 *      hasta p_insp. El volumen resultante depende del pulmón.
 * VCV (control por volumen): en la inspiración, un PI mueve el soplador para que el flujo sea
 *      constante: vt / t_flow durante el 80 % de Ti; el 20 % restante es una pausa inspiratoria (el
 *      soplador se baja y la antirretorno impide que el aire vuelva). La presión resultante depende
 *      del pulmón. Si Paw supera p_limit, la inspiración se corta (límite de presión).
 * Espiración (ambos): soplador a la PEEP y válvula espiratoria abierta con la válvula de PEEP a la PEEP.
 *
 * El tiempo es el reloj medido, no un contador de ciclos: un ciclo tardío no alarga la respiración. */
#ifndef VENT_H
#define VENT_H

#include "bg_types.h"

enum { PHASE_INSP = 0, PHASE_EXP = 1, PHASE_PAUSE = 2 };
enum { MODE_PCV = 0, MODE_VCV = 1 };

#define VCV_FLOW_FRACTION 0.8 /* parte de Ti con flujo; el resto es pausa inspiratoria */
#define PCV_HEADROOM 8.0      /* PCV: el soplador no pasa de p_insp + esto (cmH2O) */

typedef struct {
    int mode;
    vent_settings_t set;
    double vt;        /* VCV: volumen corriente objetivo (L) */
    double p_limit;   /* VCV: presión máxima; si se supera, se corta la inspiración (cmH2O) */
    double pbl_max;   /* para convertir presión en comando del soplador */
    double r_in;      /* VCV: resistencia estimada de la rama inspiratoria, para la prealimentación */
    double kp, ki;    /* PI de presión (PCV) */
    double kq, kiq;   /* PI de flujo (VCV): cmH2O por L/s y por L */
    double t0;        /* inicio de la primera respiración */
    double integ;     /* término integral (cmH2O) */
    int phase;
    int limited;      /* 1 si esta inspiración se cortó por presión */
    uint32_t breath;  /* número de respiración actual */
    uint32_t limit_count;
    double target;    /* objetivo del último paso: presión (PCV) o flujo (VCV), para registrar */
} vent_t;

void vent_init_pcv(vent_t *c, const vent_settings_t *set, double pbl_max, double now_s);
void vent_init_vcv(vent_t *c, const vent_settings_t *set, double vt, double p_limit, double pbl_max,
                   double now_s);

/* Un ciclo de control: con los sensores, calcula los actuadores. dt es el tiempo medido desde el
 * ciclo anterior. */
void vent_step(vent_t *c, double now_s, double dt, const sensors_t *s, actuators_t *out);

#endif
