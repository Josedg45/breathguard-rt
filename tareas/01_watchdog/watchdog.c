/* watchdog.c - lógica del watchdog de safety_mon (tarea 1). Las reglas están en README.md; la prueba es
 * test_watchdog.c. Sin APIs de QNX: el tiempo llega como argumento, así se prueba con un reloj simulado. */
#include "watchdog.h"

/* ¿Pasó MÁS de limit_ns desde since_ns? Con uint64_t, now - since daría un número enorme si now < since
 * (regla 5), así que primero se descarta ese caso. Justo en el límite todavía no ha expirado (regla 3). */
static int expired(uint64_t now_ns, uint64_t since_ns, uint64_t limit_ns)
{
    return now_ns > since_ns && now_ns - since_ns > limit_ns;
}

void wd_arm(watchdog_t *wd, uint64_t now_ns, uint64_t timeout_ns, uint64_t startup_ns)
{
    wd->timeout_ns = timeout_ns;
    wd->startup_ns = startup_ns;
    wd->armed_at_ns = now_ns;
    wd->last_beat_ns = now_ns;
    wd->last_seq = 0;          /* no significa nada mientras state == WD_WAITING */
    wd->state = WD_WAITING;
}

void wd_beat(watchdog_t *wd, uint64_t now_ns, uint32_t seq)
{
    /* Regla 4: un latido que llega cuando el plazo ya venció no puede borrar el fallo, aunque nadie haya
     * llamado todavía a wd_check. Por eso se evalúa el plazo ANTES de aceptar el latido. */
    if (wd_check(wd, now_ns) == WD_TIMEOUT)
        return;

    if (wd->state == WD_OK && seq == wd->last_seq)
        return;                /* regla 2: secuencia repetida = el controlador no avanzó */

    /* El primer latido vale con cualquier secuencia (también 0): el estado, no last_seq, dice si ya hubo uno. */
    wd->last_seq = seq;
    wd->last_beat_ns = now_ns;
    wd->state = WD_OK;
}

wd_state_t wd_check(watchdog_t *wd, uint64_t now_ns)
{
    if (wd->state == WD_WAITING && expired(now_ns, wd->armed_at_ns, wd->startup_ns))
        wd->state = WD_TIMEOUT;   /* regla 1: nunca arrancó */
    else if (wd->state == WD_OK && expired(now_ns, wd->last_beat_ns, wd->timeout_ns))
        wd->state = WD_TIMEOUT;   /* regla 3: dejó de latir */
    return wd->state;              /* WD_TIMEOUT se queda (regla 4) */
}
