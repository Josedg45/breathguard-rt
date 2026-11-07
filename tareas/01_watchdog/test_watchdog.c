/* test_watchdog.c - prueba de la tarea 1. Usa un reloj simulado, así que no depende de la VM. */
#include <stdio.h>

#include "watchdog.h"

#define MS 1000000ULL
#define TIMEOUT (50 * MS)
#define STARTUP (500 * MS)
#define T0 (1000 * MS)

static int failures;

static const char *name(wd_state_t s)
{
    return s == WD_WAITING ? "WD_WAITING" : s == WD_OK ? "WD_OK" : s == WD_TIMEOUT ? "WD_TIMEOUT" : "?";
}

static void expect(const char *title, wd_state_t got, wd_state_t want, const char *hint)
{
    if (got == want) {
        printf("  OK     %s\n", title);
    } else {
        printf("  FALLA  %s\n         obtuve %s, esperaba %s. Pista: %s\n", title, name(got), name(want), hint);
        failures++;
    }
}

int main(void)
{
    watchdog_t wd;

    puts("Arranque");
    wd_arm(&wd, T0, TIMEOUT, STARTUP);
    expect("recién armado espera", wd_check(&wd, T0), WD_WAITING, "regla 1");
    expect("sin latido, antes del límite de arranque sigue esperando",
           wd_check(&wd, T0 + STARTUP), WD_WAITING, "regla 1: justo en el límite aún no ha expirado");
    expect("sin latido, pasado el límite de arranque falla",
           wd_check(&wd, T0 + STARTUP + 1), WD_TIMEOUT, "regla 1");

    puts("Latidos normales");
    wd_arm(&wd, T0, TIMEOUT, STARTUP);
    wd_beat(&wd, T0 + 10 * MS, 1);
    expect("el primer latido pasa a OK", wd_check(&wd, T0 + 10 * MS), WD_OK, "regla 2");
    /* Un límite de arranque mucho mayor que el timeout no debe proteger al controlador ya en marcha. */
    expect("tras el arranque manda el timeout, no el límite de arranque",
           wd_check(&wd, T0 + 10 * MS + TIMEOUT + 1), WD_TIMEOUT, "regla 3");

    wd_arm(&wd, T0, TIMEOUT, STARTUP);
    wd_beat(&wd, T0 + 10 * MS, 1);
    wd_beat(&wd, T0 + 40 * MS, 2);
    expect("justo en el timeout sigue OK", wd_check(&wd, T0 + 40 * MS + TIMEOUT), WD_OK,
           "regla 3: falla solo si se SUPERA");
    wd_beat(&wd, T0 + 40 * MS + TIMEOUT, 3);
    expect("cada latido renueva el plazo", wd_check(&wd, T0 + 40 * MS + TIMEOUT + TIMEOUT), WD_OK, "regla 3");

    puts("Controlador colgado");
    wd_arm(&wd, T0, TIMEOUT, STARTUP);
    wd_beat(&wd, T0 + 10 * MS, 7);
    wd_beat(&wd, T0 + 40 * MS, 7);
    expect("secuencia repetida no renueva el plazo", wd_check(&wd, T0 + 10 * MS + TIMEOUT + 1), WD_TIMEOUT,
           "regla 2: un hilo atascado puede repetir el mismo latido");

    wd_arm(&wd, T0, TIMEOUT, STARTUP);
    wd_beat(&wd, T0 + 10 * MS, 0);
    expect("la secuencia 0 también vale como primer latido", wd_check(&wd, T0 + 10 * MS), WD_OK,
           "regla 2: ¿estás usando last_seq == 0 para saber si ya hubo un latido?");

    puts("El fallo no se cura solo");
    wd_arm(&wd, T0, TIMEOUT, STARTUP);
    wd_beat(&wd, T0 + 10 * MS, 1);
    wd_check(&wd, T0 + 10 * MS + TIMEOUT + 1);
    wd_beat(&wd, T0 + 10 * MS + TIMEOUT + 2, 2);
    expect("un latido tardío no revive el watchdog", wd_check(&wd, T0 + 10 * MS + TIMEOUT + 2), WD_TIMEOUT, "regla 4");
    wd_arm(&wd, T0 + 2000 * MS, TIMEOUT, STARTUP);
    expect("rearmar sí lo reinicia", wd_check(&wd, T0 + 2000 * MS), WD_WAITING, "regla 4");

    puts("Tiempos fuera de orden");
    wd_arm(&wd, T0, TIMEOUT, STARTUP);
    wd_beat(&wd, T0 + 100 * MS, 1);
    expect("un latido más nuevo que now no es un timeout", wd_check(&wd, T0 + 99 * MS), WD_OK,
           "regla 5: con uint64_t, 99 - 100 no da -1");

    if (failures) {
        printf("\nRESULTADO: %d prueba(s) fallaron\n", failures);
        return 1;
    }
    puts("\nRESULTADO: todas las pruebas pasaron");
    return 0;
}
