/* test_vt_meter.c - unit test of src/vt_meter.c (rules in vt_meter.h). Uses synthetic flows. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "vt_meter.h"

static int failures;

static void expect_near(const char *title, double got, double want, double tol, const char *hint)
{
    if (fabs(got - want) <= tol) {
        printf("  OK    %s (%.4f)\n", title, got);
    } else {
        printf("  FAIL  %s\n        got %.6f, expected %.6f +/- %g (%s)\n", title, got, want, tol, hint);
        failures++;
    }
}

static void expect_u(const char *title, unsigned got, unsigned want, const char *hint)
{
    if (got == want) {
        printf("  OK    %s (%u)\n", title, got);
    } else {
        printf("  FAIL  %s\n        got %u, expected %u (%s)\n", title, got, want, hint);
        failures++;
    }
}

/* Feeds one breath: inspiration of ti s with flow qi(t) and expiration of te s with qe(t).
 * Without jitter, samples every exact dt; with jitter, every dt*(1..2), like the VM losing ticks.
 * Returns the time of the next sample. */
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
static double q_ramp(double t) { return 0.6 * t; }                 /* ramp: volume = 0.3*t^2 */
static double q_exp_decay(double t) { return -0.4 * exp(-t / 0.5); }

int main(void)
{
    vt_meter_t m;
    srand(1234);

    puts("Before the first inspiration");
    vtm_init(&m);
    vtm_update(&m, 0.000, -0.1, 0);
    vtm_update(&m, 0.001, -0.1, 0);
    expect_u("no breaths yet", m.breaths, 0, "rule 6");
    expect_near("Vti still 0", m.vti, 0.0, 1e-12, "rule 6");

    puts("Constant flow, regular 1 ms sampling");
    vtm_init(&m);
    double t = feed(&m, 0.0, 1.0, q_const, 2.0, q_exp_const, 0.001, 0);
    expect_near("Vti = 0.5 L/s * 1 s", m.vti, 0.5, 1.5e-3, "rule 2");
    vtm_update(&m, t, 0.5, 1); /* the next breath starts */
    expect_near("Vte = 0.25 L/s * 2 s", m.vte, 0.5, 1.5e-3, "rule 3");
    expect_u("one complete breath", m.breaths, 1, "rule 3");

    puts("Coarse 10 ms sampling: trapezoid, not rectangle");
    vtm_init(&m);
    t = feed(&m, 0.0, 1.0, q_ramp, 2.0, q_exp_decay, 0.010, 0);
    /* Trapezoid with samples at 0, 0.01 ... 0.99 (inspiration) and the interval [0.99, 1.00] that ends
     * at the first expiration sample (flow -0.4): 0.3*0.99^2 + (0.594 - 0.4)/2*0.01 */
    expect_near("Vti of a ramp, trapezoidal", m.vti, 0.3 * 0.99 * 0.99 + (0.594 - 0.4) / 2 * 0.01, 1e-6,
                "rules 1 and 2");

    puts("Irregular sampling (lost ticks: dt between 1 and 2 ms)");
    vtm_init(&m);
    t = feed(&m, 0.0, 1.0, q_const, 2.0, q_exp_decay, 0.001, 1);
    vtm_update(&m, t, 0.0, 1);
    expect_near("Vti with irregular dt", m.vti, 0.5, 2e-3, "rule 1");
    expect_near("Vte with irregular dt", m.vte, 0.4 * 0.5 * (1 - exp(-4.0)), 3e-3, "rule 1");

    puts("Several breaths in a row");
    vtm_init(&m);
    t = 0;
    for (int i = 0; i < 3; i++) t = feed(&m, t, 1.0, q_const, 2.0, q_exp_const, 0.001, 1);
    vtm_update(&m, t, 0.5, 1);
    expect_u("three breaths", m.breaths, 3, "rule 3");
    expect_near("Vti does not accumulate across breaths", m.vti, 0.5, 2e-3, "rule 3");

    puts("Leak: less comes out than goes in");
    vtm_init(&m);
    t = feed(&m, 0.0, 1.0, q_const, 2.0, q_exp_const, 0.001, 0);
    t = feed(&m, t, 1.0, q_const, 2.0, q_exp_decay, 0.001, 0);
    vtm_update(&m, t, 0.5, 1);
    expect_near("normal Vti", m.vti, 0.5, 1.5e-3, "rule 2");
    expect_near("smaller Vte", m.vte, 0.4 * 0.5 * (1 - exp(-4.0)), 2e-3, "rule 3");

    puts("Sampling gap (the VM freezes for 80 ms)");
    vtm_init(&m);
    vtm_update(&m, 0.000, 0.5, 1);
    vtm_update(&m, 0.001, 0.5, 1);
    vtm_update(&m, 0.081, 0.5, 1); /* 80 ms without samples */
    vtm_update(&m, 0.082, 0.5, 1);
    vtm_update(&m, 0.083, 0.0, 0);
    expect_near("the gap is not integrated", m.vti, 0.5 * 0.002 + 0.25 * 0.001, 1e-9, "rule 4");
    expect_u("the gap is counted", m.gaps, 1, "rule 4");

    puts("Time that does not move forward");
    vtm_init(&m);
    vtm_update(&m, 1.000, 0.5, 1);
    vtm_update(&m, 1.001, 0.5, 1);
    vtm_update(&m, 1.001, 0.5, 1); /* repeated sample */
    vtm_update(&m, 0.990, 0.5, 1); /* clock going backwards */
    vtm_update(&m, 0.991, 0.5, 1);
    vtm_update(&m, 0.992, 0.0, 0);
    expect_near("intervals <= 0 are not integrated", m.vti, 0.5 * 0.001 + 0.5 * 0.001 + 0.25 * 0.001, 1e-9,
                "rule 5");
    expect_u("both bad intervals are counted", m.gaps, 2, "rule 5");

    if (failures) {
        printf("\nRESULT: %d check(s) failed\n", failures);
        return 1;
    }
    puts("\nRESULT: all checks passed");
    return 0;
}
