/* pcv.h - control por presión (PCV) de breath_ctrl. C puro, sin QNX.
 *
 * Inspiración: válvula espiratoria cerrada; un PI mueve el soplador para que Paw siga una rampa
 *              hasta p_insp.
 * Espiración:  soplador a la PEEP y válvula espiratoria abierta con la válvula de PEEP a la PEEP.
 * El tiempo es el reloj medido, no un contador de ciclos: si un ciclo llega tarde, la respiración
 * no se alarga. */
#ifndef PCV_H
#define PCV_H

#include "bg_types.h"

enum { PHASE_INSP = 0, PHASE_EXP = 1 };

typedef struct {
    vent_settings_t set;
    double pbl_max;   /* para convertir presión en comando del soplador */
    double kp, ki;    /* ganancias del PI (cmH2O de soplador por cmH2O de error, y por cmH2O·s) */
    double t0;        /* inicio de la primera respiración */
    double integ;     /* término integral (cmH2O) */
    int phase;
    uint32_t breath;  /* número de respiración actual */
    double target;    /* presión objetivo del último paso (para registrar) */
} pcv_t;

void pcv_init(pcv_t *c, const vent_settings_t *set, double pbl_max, double now_s);

/* Un ciclo de control: con los sensores, calcula los actuadores. dt es el tiempo medido desde el
 * ciclo anterior. */
void pcv_step(pcv_t *c, double now_s, double dt, const sensors_t *s, actuators_t *out);

#endif
