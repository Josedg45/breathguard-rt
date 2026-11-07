/* vt_meter.c - TU TAREA 2. Las reglas están en README.md; la prueba es test_vt_meter.c. */
#include "vt_meter.h"

#include <string.h>

void vtm_init(vt_meter_t *m)
{
    memset(m, 0, sizeof *m);
}

void vtm_update(vt_meter_t *m, double t_s, double flow, int insp)
{
    /* TODO 1: integrar el intervalo desde la muestra anterior (regla 1), salvo que no haya muestra
     *         anterior o que el intervalo no sea válido (reglas 4 y 5). */

    /* TODO 2: detectar las transiciones de fase comparando insp con m->prev_insp:
     *         - fin de la inspiración (regla 2)
     *         - inicio de una inspiración nueva (reglas 3 y 6) */

    /* TODO 3: guardar esta muestra como "anterior" para la próxima llamada. */
    (void)m; (void)t_s; (void)flow; (void)insp;
}
