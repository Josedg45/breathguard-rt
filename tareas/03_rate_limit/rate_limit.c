/* rate_limit.c - Tarea 3 (M6): limite de cambio por comando del operador. Ver README.md.
 *
 * safety_mon llama a settings_rate_ok() desde settings_validate() con los ajustes vigentes (cur) y los
 * pedidos (req), DESPUES de comprobar que req esta dentro de los limites absolutos. Devuelve 1 si el
 * salto es aceptable; si no, 0 y escribe el motivo en why (n bytes, con snprintf).
 */
#include <math.h>
#include <stdio.h>

#include "settings.h"

int settings_rate_ok(const bg_settings_t *cur, const bg_settings_t *req, char *why, size_t n)
{
    (void)cur;
    (void)req;
    (void)why;
    (void)n;
    /* TODO 1: si req->vcv == cur->vcv y es PCV, |req p_insp - cur p_insp| <= RATE_P_INSP; si es VCV,
     *         |req vt - cur vt| <= RATE_VT. Si cambia el modo, no se aplica esta regla. */
    /* TODO 2: |PEEP| <= RATE_PEEP y |rr| <= RATE_RR, siempre. */
    /* TODO 3: el motivo del rechazo en why, p. ej. "p_insp cambia 10.0 > 5 cmH2O por comando". */
    return 1; /* esqueleto: acepta todo */
}
