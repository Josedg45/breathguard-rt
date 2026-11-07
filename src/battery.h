/* battery.h - modelo de la batería y de los consumos del ventilador (power_mgr). C puro, sin QNX.
 *
 * Batería de ion-litio 7S (24 V nominal) con circuito equivalente de Thevenin de primer orden:
 *
 *     Voc(SoC) ──R0──┬──R1──┬── V terminal
 *                    └──C1──┘
 *   dSoC/dt = -I / (3600·Q)        I > 0 = descarga (A), Q en Ah
 *   dV1/dt  = -V1/(R1·C1) + I/C1   (polarización: la caída que tarda en aparecer y en irse)
 *   V       = Voc(SoC) - I·R0 - V1
 * Con red eléctrica, la fuente alimenta al ventilador y carga la batería a corriente constante. */
#ifndef BATTERY_H
#define BATTERY_H

typedef struct {
    double q_ah;     /* capacidad (Ah) */
    double r0, r1, c1;
    double i_charge; /* corriente de carga con red (A) */
} battery_params_t;

typedef struct {
    battery_params_t p;
    double soc;      /* estado de carga, 0..1 */
    double v1;       /* tensión de la rama RC (V) */
    double i;        /* corriente actual (A, > 0 descarga) */
    double v;        /* tensión en bornes (V) */
} battery_t;

battery_params_t battery_default_params(void);
void battery_init(battery_t *b, const battery_params_t *p, double soc);

/* Tensión de circuito abierto de la batería para un SoC (curva típica de 7 celdas de ion-litio). */
double battery_ocv(double soc);

/* Avanza dt segundos entregando p_load vatios (sin red) o cargando (con red). */
void battery_step(battery_t *b, double p_load_w, int mains, double dt);

/* Energía utilizable (Wh) desde el SoC actual hasta soc_min, sin contar pérdidas en R0. */
double battery_energy_wh(const battery_t *b, double soc_min);

/* Consumos del ventilador (W). */
typedef struct {
    double base;       /* control y sensores: siempre */
    double blower_idle;/* soplador girando sin carga */
    double blower_eff; /* eficiencia neumática del soplador (0..1) */
    double display;    /* pantalla a brillo normal */
    double display_dim;/* pantalla atenuada */
    double humidifier; /* humidificador calefactado: no esencial en batería */
    double telemetry;  /* radio y telemetría */
} loads_t;

loads_t loads_default(void);

/* Potencia total (W) según la potencia neumática del soplador (Paw·flujo) y si se recortan los no esenciales. */
double loads_power(const loads_t *l, double paw_cmh2o, double flow_lps, int shed);

#endif
