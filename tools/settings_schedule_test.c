/* settings_schedule_test.c - prueba en el host de settings_schedule() (src/settings.c): cuando empieza un cambio de
 * ajustes y cuando se rechaza como "busy" por estar a menos de SET_LEAD_S de empezar. Escrita por Santiago en la rama
 * fix/settings-handover-race (estaba en el README como comprobacion sugerida).
 * Uso: make build/settings_schedule_test && build/settings_schedule_test */
#include <stdio.h>
#include <string.h>

#include "settings.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void)
{
    /* Breaths every 5 s (12 bpm) from t = 10 s. */
    bg_settings_t cur = { .seq = 1, .set = { .rr = 12, .ie = 2, .p_insp = 20, .peep = 5 }, .t0 = 10.0 };
    bg_settings_t next = cur;
    char why[64] = "";
    double t0 = -1;

    /* No pending change: the first breath start at least SET_LEAD_S (0.2 s) away. */
    CHECK(settings_schedule(&cur, NULL, 14.7, &t0, why, sizeof why) && t0 == 15.0);
    CHECK(settings_schedule(&cur, NULL, 14.9, &t0, why, sizeof why) && t0 == 20.0);

    /* Pending change 0.3 s away: replaced in place, same t0. */
    next.seq = 2;
    next.t0 = 15.0;
    CHECK(settings_schedule(&cur, &next, 14.7, &t0, why, sizeof why) && t0 == 15.0);

    /* Pending change 0.15 s away, or already due: refused as busy; no comma (it goes to the events CSV). */
    t0 = -1;
    CHECK(!settings_schedule(&cur, &next, 14.85, &t0, why, sizeof why) && t0 == -1);
    CHECK(strncmp(why, SET_BUSY_PREFIX, strlen(SET_BUSY_PREFIX)) == 0 && !strchr(why, ','));
    CHECK(!settings_schedule(&cur, &next, 15.0, &t0, why, sizeof why));
    printf("%s\nsettings_schedule: all checks passed\n", why);
    return 0;
}
