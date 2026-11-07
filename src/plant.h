/* plant.h - física del circuito neumático y del pulmón (pneumatic_sim). C puro, sin QNX.
 *
 *   soplador --R_in, válvula antirretorno--> [pieza en Y, C_tube] --R_pat--> pulmón (C_pat)
 *                                                 |
 *                                                 +--> válvula espiratoria + PEEP --R_ex--> aire
 *
 * Estados: presión del soplador, presión en la Y (Paw) y volumen del pulmón. */
#ifndef PLANT_H
#define PLANT_H

#include "bg_types.h"

typedef struct {
    double pbl_max;    /* presión máxima del soplador */
    double tau_bl;     /* constante de tiempo del soplador (s) */
    double r_in;       /* rama inspiratoria */
    double c_tube;     /* compliance de la tubuladura */
    double r_ex;       /* rama espiratoria con la válvula abierta */
    double r_disc;     /* salida al aire si se desconecta el tubo */
    double r_pat;      /* resistencia del paciente */
    double c_pat;      /* compliance del paciente */
    double v0;         /* volumen del pulmón con presión alveolar 0 (L) */
    double noise_paw;  /* desviación del ruido del sensor de presión (cmH2O) */
    double noise_flow; /* desviación del ruido del sensor de flujo (L/s) */
} plant_params_t;

typedef struct {
    plant_params_t p;
    plant_faults_t f;
    double t;       /* tiempo simulado (s) */
    double pbl;     /* presión del soplador */
    double paw;     /* presión en la Y */
    double v;       /* volumen absoluto del pulmón (L) */
    double q_in, q_ex, q_y, q_lung; /* flujos del último subpaso (L/s) */
    double paw_int; /* integral de la presión en la boca del paciente (cmH2O·s), para Pulse: es Paw si
                     * el tubo está conectado y 0 (atmósfera) si está desconectado */
    double paw_last_read;
    uint32_t seq;
    uint64_t rng;
} plant_t;

/* Valores por defecto: adulto sedado parecido al paciente de Pulse (tau = R·C ≈ 1.3 s). */
plant_params_t plant_default_params(void);

void plant_init(plant_t *pl, const plant_params_t *p, double peep);

/* Avanza dt segundos con los actuadores dados. Usa subpasos de 50 µs, porque la tubuladura es rápida. */
void plant_advance(plant_t *pl, const actuators_t *a, double dt);

/* Lee los sensores (con ruido y fallos). */
void plant_sense(plant_t *pl, sensors_t *s);

/* Presión alveolar actual (cmH2O). */
double plant_palv(const plant_t *pl);

#endif
