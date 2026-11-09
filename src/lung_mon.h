/* lung_mon.h - mecanica respiratoria por respiracion a partir de los sensores de la pieza en Y (safety_mon).
 * C puro, sin QNX: se prueba en el host con la planta (tools/lung_mon_test.c).
 *
 * Auto-PEEP (PEEP intrinseca): si la espiracion no dura lo suficiente, el pulmon no termina de vaciarse y el
 * flujo espiratorio no llega a cero. La presion alveolar al final de la espiracion queda por encima de la PEEP
 * puesta. Un ventilador real lo mide con una pausa espiratoria (cierra las valvulas y lee la presion); aqui se
 * estima sin maniobra, con un modelo de un compartimento (pulmon R-C) en la espiracion:
 *
 *   Palv(t) - PEEP = |Q(t)| * R_tot          R_tot = R_pac + R_circ (paciente + rama espiratoria)
 *   |Q(t)| = |Q0| * exp(-t / tau)            tau = R_tot * C
 *
 *   1. tau y Q0: recta de minimos cuadrados de ln|Q| contra t en la espiracion (Q0 = flujo extrapolado al
 *      instante en que empieza la espiracion).
 *   2. R_circ: (Paw - PEEP) / |Q| en la espiracion (la presion en la Y es lo que "cae" en la rama espiratoria).
 *   3. R_pac: la presion alveolar es continua al pasar de inspiracion a espiracion:
 *        Paw_ei - Q_ei * R_pac = PEEP + |Q0| * (R_pac + R_circ)
 *      (ei = final de la inspiracion; en PCV el flujo inspiratorio aun no es cero si tau es largo).
 *   4. auto-PEEP = |Q_ee| * R_tot (ee = final de la espiracion), y C = tau / R_tot.
 * Limites: un solo compartimento (un pulmon con zonas de tau distintas no da una exponencial), sin esfuerzo
 * del paciente (sedado) y sin fuga. Por eso tambien se informa el criterio directo: flujo al final de la
 * espiracion distinto de cero (eef). */
#ifndef LUNG_MON_H
#define LUNG_MON_H

#include <stdint.h>

typedef struct {
    double q_eef;         /* L/s: |Q_ee| por encima de esto = el flujo no vuelve a cero */
    double auto_peep_max; /* cmH2O: alarma de auto-PEEP (2 respiraciones seguidas por encima) */
} lung_cfg_t;

#define LM_RING 64

typedef struct {
    lung_cfg_t cfg;
    int prev_insp, have_prev;
    double t_exp0;               /* inicio de la espiracion en curso (s); < 0 si no hay */
    double peep;                 /* PEEP puesta de la respiracion en curso */
    /* Final de la inspiracion: ultimas muestras de Paw y Q */
    double ring_p[LM_RING], ring_q[LM_RING], ring_t[LM_RING];
    unsigned ri;
    double paw_ei, q_ei;
    /* Ajuste exponencial y R_circ, acumulados durante la espiracion */
    double sx, sy, sxx, sxy, n_fit;
    double s_pq, s_qq;
    /* Resultado de la ultima respiracion completa */
    int valid;
    double auto_peep, q_ee, tau, r_tot, r_circ, c;
    int eef;
    uint32_t breaths, over;      /* respiraciones evaluadas; seguidas por encima del limite */
    int alarm;
} lung_mon_t;

/* Ventilacion protectora: Vt de 6-8 mL por kg de peso IDEAL (depende de la talla y el sexo, no del peso real:
 * el tamano del pulmon sigue a la talla). Formula de ARDSNet (Devine). */
#define VT_PROTECT_LO_ML_KG 6.0
#define VT_PROTECT_HI_ML_KG 8.0
static inline double ibw_kg(double height_cm, int male)
{
    return (male ? 50.0 : 45.5) + 0.91 * (height_cm - 152.4);
}

lung_cfg_t lung_default_cfg(void);
void lung_init(lung_mon_t *m, const lung_cfg_t *cfg);

/* Una muestra cada 1 ms: tiempo (s), Paw (cmH2O), flujo hacia el paciente (L/s), fase segun el calendario
 * (1 = inspiracion, incluida la pausa) y PEEP puesta. Devuelve 1 si acaba de cerrar una respiracion. */
int lung_update(lung_mon_t *m, double t, double paw, double flow, int insp, double peep);

#endif
