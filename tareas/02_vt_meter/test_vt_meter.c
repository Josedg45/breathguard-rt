/* test_vt_meter.c - prueba de la tarea 2. Usa flujos sintéticos, así que no depende de la VM. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "vt_meter.h"

static int failures;

static void expect_near(const char *title, double got, double want, double tol, const char *hint)
{
    if (fabs(got - want) <= tol) {
        printf("  OK     %s (%.4f)\n", title, got);
    } else {
        printf("  FALLA  %s\n         obtuve %.6f, esperaba %.6f ± %g. Pista: %s\n", title, got, want, tol, hint);
        failures++;
    }
}

static void expect_u(const char *title, unsigned got, unsigned want, const char *hint)
{
    if (got == want) {
        printf("  OK     %s (%u)\n", title, got);
    } else {
        printf("  FALLA  %s\n         obtuve %u, esperaba %u. Pista: %s\n", title, got, want, hint);
        failures++;
    }
}

/* Alimenta una respiración: inspiración de ti s con flujo qi(t) y espiración de te s con qe(t).
 * Sin jitter, muestrea cada dt exacto; con jitter, cada dt·(1..2), como la VM que pierde ticks.
 * Devuelve el tiempo de la muestra siguiente. */
typedef double (*flow_fn)(double t);
static double feed(vt_meter_t *m, double t0, double ti, flow_fn qi, double te, flow_fn qe, double dt, int jitter)
{
    double t = t0;
    long i = 0;
    while (t < t0 + ti + te - 1e-9) {
        int insp = t < t0 + ti - 1e-9;
        double q = insp ? qi(t - t0) : qe(t - t0 - ti);
        vtm_update(m, t, q, insp);
        i++;
        t = jitter ? t + dt * (1.0 + (double)rand() / RAND_MAX) : t0 + (double)i * dt;
    }
    return t;
}

static double q_const(double t) { (void)t; return 0.5; }          /* 0.5 L/s */
static double q_exp_const(double t) { (void)t; return -0.25; }    /* -0.25 L/s */
static double q_ramp(double t) { return 0.6 * t; }                 /* rampa: volumen = 0.3·t² */
static double q_exp_decay(double t) { return -0.4 * exp(-t / 0.5); }

int main(void)
{
    vt_meter_t m;
    srand(1234);

    puts("Antes de la primera inspiración");
    vtm_init(&m);
    vtm_update(&m, 0.000, -0.1, 0);
    vtm_update(&m, 0.001, -0.1, 0);
    expect_u("sin respiraciones aún", m.breaths, 0, "regla 6: lo que pasa antes del primer inicio no cuenta");
    expect_near("Vti aún 0", m.vti, 0.0, 1e-12, "regla 6");

    puts("Flujo constante, muestreo regular de 1 ms");
    vtm_init(&m);
    double t = feed(&m, 0.0, 1.0, q_const, 2.0, q_exp_const, 0.001, 0);
    expect_near("Vti = 0.5 L/s · 1 s", m.vti, 0.5, 1.5e-3, "regla 2: Vti se fija al pasar insp de 1 a 0");
    vtm_update(&m, t, 0.5, 1); /* empieza la respiración siguiente */
    expect_near("Vte = 0.25 L/s · 2 s", m.vte, 0.5, 1.5e-3, "regla 3: Vte en positivo");
    expect_u("una respiración completa", m.breaths, 1, "regla 3");

    puts("Muestreo grueso de 10 ms: ¿trapecio o rectángulo?");
    vtm_init(&m);
    t = feed(&m, 0.0, 1.0, q_ramp, 2.0, q_exp_decay, 0.010, 0);
    /* Trapecio con muestras en 0, 0.01 ... 0.99 (inspiración) y el intervalo [0.99, 1.00] que termina
     * en la primera muestra de espiración (flujo -0.4): 0.3·0.99² + (0.594 - 0.4)/2·0.01 */
    expect_near("Vti de una rampa, por trapecio", m.vti, 0.3 * 0.99 * 0.99 + (0.594 - 0.4) / 2 * 0.01, 1e-6,
                "regla 1 (trapecio, no rectángulo) y regla 2 (el intervalo que termina en la muestra del "
                "cambio de fase se integra ANTES de fijar Vti)");

    puts("Muestreo irregular (la VM pierde ticks: dt entre 1 y 2 ms)");
    vtm_init(&m);
    t = feed(&m, 0.0, 1.0, q_const, 2.0, q_exp_decay, 0.001, 1);
    vtm_update(&m, t, 0.0, 1);
    expect_near("Vti con dt irregular", m.vti, 0.5, 2e-3, "regla 1: usa el dt real (t - t_prev), no 1 ms fijo");
    expect_near("Vte con dt irregular", m.vte, 0.4 * 0.5 * (1 - exp(-4.0)), 3e-3, "regla 1");

    puts("Varias respiraciones seguidas");
    vtm_init(&m);
    t = 0;
    for (int i = 0; i < 3; i++) t = feed(&m, t, 1.0, q_const, 2.0, q_exp_const, 0.001, 1);
    vtm_update(&m, t, 0.5, 1);
    expect_u("tres respiraciones", m.breaths, 3, "regla 3: cada inicio de inspiración cierra la anterior");
    expect_near("Vti no se acumula entre respiraciones", m.vti, 0.5, 2e-3,
                "regla 3: el volumen vuelve a 0 al empezar cada respiración");

    puts("Fuga: sale menos de lo que entra");
    vtm_init(&m);
    t = feed(&m, 0.0, 1.0, q_const, 2.0, q_exp_const, 0.001, 0);
    t = feed(&m, t, 1.0, q_const, 2.0, q_exp_decay, 0.001, 0);
    vtm_update(&m, t, 0.5, 1);
    expect_near("Vti normal", m.vti, 0.5, 1.5e-3, "regla 2");
    expect_near("Vte menor", m.vte, 0.4 * 0.5 * (1 - exp(-4.0)), 2e-3, "regla 3");

    puts("Hueco en el muestreo (la VM se congela 80 ms)");
    vtm_init(&m);
    vtm_update(&m, 0.000, 0.5, 1);
    vtm_update(&m, 0.001, 0.5, 1);
    vtm_update(&m, 0.081, 0.5, 1); /* 80 ms sin muestras */
    vtm_update(&m, 0.082, 0.5, 1);
    vtm_update(&m, 0.083, 0.0, 0);
    expect_near("no integra el hueco", m.vti, 0.5 * 0.002 + 0.25 * 0.001, 1e-9,
                "regla 4: un intervalo > VTM_MAX_GAP_S no se integra");
    expect_u("cuenta el hueco", m.gaps, 1, "regla 4");

    puts("Tiempo que no avanza");
    vtm_init(&m);
    vtm_update(&m, 1.000, 0.5, 1);
    vtm_update(&m, 1.001, 0.5, 1);
    vtm_update(&m, 1.001, 0.5, 1); /* muestra repetida */
    vtm_update(&m, 0.990, 0.5, 1); /* reloj hacia atrás */
    vtm_update(&m, 0.991, 0.5, 1);
    vtm_update(&m, 0.992, 0.0, 0);
    expect_near("no integra intervalos <= 0", m.vti, 0.5 * 0.001 + 0.5 * 0.001 + 0.25 * 0.001, 1e-9,
                "regla 5: con dt <= 0 no se integra, pero la muestra sí pasa a ser la anterior");
    expect_u("cuenta los dos intervalos malos", m.gaps, 2, "regla 5");

    if (failures) {
        printf("\nRESULTADO: %d prueba(s) fallaron\n", failures);
        return 1;
    }
    puts("\nRESULTADO: todas las pruebas pasaron");
    return 0;
}
