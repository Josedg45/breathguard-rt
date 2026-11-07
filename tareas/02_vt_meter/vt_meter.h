/* vt_meter.h - medidor de volumen corriente a partir del flujo medido. Sin APIs de QNX: el tiempo
 * se recibe como argumento, así que se puede probar con datos sintéticos.
 *
 * Una respiración empieza cuando insp pasa de 0 a 1. Durante ella se integra el flujo:
 *   Vti (volumen inspirado) = volumen integrado al terminar la inspiración (insp 1 -> 0)
 *   Vte (volumen espirado)  = lo que salió durante la espiración, en positivo
 * Si Vte es mucho menor que Vti, el aire se está escapando: fuga o desconexión. */
#ifndef VT_METER_H
#define VT_METER_H

#include <stdint.h>

#define VTM_MAX_GAP_S 0.050 /* un hueco mayor entre muestras no se integra */

typedef struct {
    int started;       /* 1 desde el primer inicio de inspiración */
    int have_prev;     /* 1 si hay una muestra anterior válida para integrar */
    int prev_insp;     /* fase de la muestra anterior */
    double t_prev;     /* tiempo de la muestra anterior (s) */
    double q_prev;     /* flujo de la muestra anterior (L/s) */
    double vol;        /* volumen integrado desde el inicio de la respiración actual (L) */
    double vol_end_insp; /* vol al terminar la inspiración de la respiración actual (L) */
    double vti, vte;   /* último Vti y Vte completos (L); 0 si aún no hay */
    uint32_t breaths;  /* respiraciones con Vte completo */
    uint32_t gaps;     /* intervalos no integrados por hueco o por tiempo que no avanza */
} vt_meter_t;

void vtm_init(vt_meter_t *m);

/* Una muestra: tiempo (s), flujo hacia el paciente (L/s, negativo al espirar) y fase
 * (insp = 1 en inspiración, incluida la pausa; 0 en espiración). */
void vtm_update(vt_meter_t *m, double t_s, double flow, int insp);

#endif
