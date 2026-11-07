/* watchdog.c - TU TAREA. Las reglas están en README.md; la prueba es test_watchdog.c. */
#include "watchdog.h"

void wd_arm(watchdog_t *wd, uint64_t now_ns, uint64_t timeout_ns, uint64_t startup_ns)
{
    /* TODO 1: guarda la configuración y deja el watchdog listo en WD_WAITING. */
    (void)wd; (void)now_ns; (void)timeout_ns; (void)startup_ns;
}

void wd_beat(watchdog_t *wd, uint64_t now_ns, uint32_t seq)
{
    /* TODO 2: ¿este latido cuenta? Piensa en las reglas 2 y 4 antes de escribir nada.
     * TODO 3: si cuenta, actualiza lo necesario. */
    (void)wd; (void)now_ns; (void)seq;
}

wd_state_t wd_check(watchdog_t *wd, uint64_t now_ns)
{
    /* TODO 4: según el estado actual, ¿contra qué instante y qué límite comparas?
     * TODO 5: decide si hay que pasar a WD_TIMEOUT. Cuidado con la regla 5: estos números no
     *         tienen signo. */
    (void)now_ns;
    return wd->state;
}
