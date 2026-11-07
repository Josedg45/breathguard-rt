/* bg_types.h - tipos compartidos de BreathGuard RT. Sin APIs de QNX.
 * Unidades en todo el proyecto: presión cmH2O (relativa a la atmósfera), volumen L, flujo L/s,
 * resistencia cmH2O/(L/s), compliance L/cmH2O, tiempo s. */
#ifndef BG_TYPES_H
#define BG_TYPES_H

#include <stdint.h>

/* Lo que el controlador manda a la planta (actuadores). */
typedef struct {
    double blower;   /* soplador, 0..1 de su presión máxima */
    double exh_open; /* válvula espiratoria, 0 cerrada .. 1 abierta */
    double peep_cmd; /* presión de apertura de la válvula de PEEP (cmH2O) */
} actuators_t;

/* Lo que la planta entrega al controlador (sensores, con ruido). */
typedef struct {
    uint32_t seq;   /* contador de lecturas */
    double t_s;     /* tiempo de la planta al leer */
    double paw;     /* presión en la pieza en Y (cmH2O) */
    double flow;    /* flujo en la pieza en Y hacia el paciente (L/s) */
} sensors_t;

/* Fallos inyectables en la planta. */
typedef struct {
    int disconnected;   /* 1 = tubo desconectado del paciente */
    double obstruction; /* multiplica la resistencia del paciente (1 = normal) */
    double g_leak;      /* conductancia de fuga (L/s por cmH2O); 0 = sin fuga */
    int paw_stuck;      /* 1 = el sensor de presión se congela en su último valor */
} plant_faults_t;

/* Ajustes del ventilador en PCV. */
typedef struct {
    double rr;       /* frecuencia respiratoria (resp/min) */
    double ie;       /* relación I:E expresada como Te/Ti (2 = 1:2) */
    double p_insp;   /* presión inspiratoria total sobre la atmósfera (cmH2O) */
    double peep;     /* PEEP (cmH2O) */
    double rise_s;   /* tiempo de subida de la presión al inicio de la inspiración */
} vent_settings_t;

#endif
