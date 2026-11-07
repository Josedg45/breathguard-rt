/* watchdog.h - lógica del watchdog de safety_mon. Sin APIs de QNX: el tiempo se recibe como
 * argumento, así que se puede probar con un reloj simulado. */
#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WD_WAITING, /* arrancado, aún sin el primer latido válido */
    WD_OK,      /* recibiendo latidos a tiempo */
    WD_TIMEOUT  /* el controlador falló: hay que ir a estado seguro */
} wd_state_t;

typedef struct {
    uint64_t timeout_ns;   /* máximo permitido entre latidos */
    uint64_t startup_ns;   /* máximo permitido hasta el primer latido */
    uint64_t armed_at_ns;  /* cuándo se armó el watchdog */
    uint64_t last_beat_ns; /* cuándo llegó el último latido válido */
    uint32_t last_seq;     /* número de secuencia del último latido válido */
    wd_state_t state;
} watchdog_t;

/* Arma el watchdog en estado WD_WAITING. */
void wd_arm(watchdog_t *wd, uint64_t now_ns, uint64_t timeout_ns, uint64_t startup_ns);

/* Registra un latido del controlador, con su número de secuencia. */
void wd_beat(watchdog_t *wd, uint64_t now_ns, uint32_t seq);

/* Evalúa el tiempo transcurrido, actualiza el estado y lo devuelve. */
wd_state_t wd_check(watchdog_t *wd, uint64_t now_ns);

#endif
