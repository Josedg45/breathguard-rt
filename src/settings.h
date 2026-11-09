/* settings.h - M6: ajustes del ventilador que el operador puede cambiar en marcha. C puro, sin QNX.
 *
 * Quien manda sobre los ajustes es safety_mon (no la HMI, que es prescindible y no confiable):
 *   1. La HMI recibe un comando del operador y lo reenvia a safety_mon.
 *   2. safety_mon lo valida (settings_validate). Si es valido, lo programa para el inicio de la proxima
 *      respiracion (settings_next_breath): t0 = instante desde el que valen, comun a todos los procesos.
 *   3. breath_ctrl consulta los ajustes cada 100 ms y aplica los nuevos justo en t0; el respaldo de
 *      safety_mon cambia en el mismo instante. Asi nunca ventilan con ajustes distintos ni fuera de fase.
 *   4. (settings_schedule) A pending change may be replaced by a newer command only while its t0 is at least
 *      SET_LEAD_S away. Closer than that, breath_ctrl may already hold the pending change and not poll again before
 *      t0, so the newer command is refused as "busy" and the operator resends it once the pending change has started.
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stddef.h>
#include <stdint.h>

#include "bg_types.h"

typedef struct {
    uint32_t seq;          /* sube con cada cambio aceptado (0 = ajustes de arranque) */
    int32_t vcv;           /* 0 PCV, 1 VCV */
    vent_settings_t set;   /* rr, ie, p_insp, peep, rise_s */
    double vt;             /* VCV: volumen corriente (L) */
    double t0;             /* inicio de la respiracion desde la que valen (s, reloj comun rt_now_s) */
} bg_settings_t;

/* Limites que safety_mon acepta. Fuera de ellos el comando se rechaza entero. */
#define SET_P_INSP_MIN 5.0
#define SET_P_INSP_MAX 35.0   /* por debajo de la alarma de presion alta (38) con margen */
#define SET_PEEP_MIN 0.0
#define SET_PEEP_MAX 15.0
#define SET_DRIVE_MIN 5.0     /* p_insp - peep minima */
#define SET_RR_MIN 6.0
#define SET_RR_MAX 30.0
#define SET_IE_MIN 1.0        /* I:E 1:1 */
#define SET_IE_MAX 4.0        /* I:E 1:4 */
#define SET_VT_MIN 0.2        /* L */
#define SET_VT_MAX 0.8
#define SET_LEAD_S 0.2        /* un cambio necesita al menos esto antes de aplicarse */
                              /* Must stay above breath_ctrl's poll period (SETTINGS_EVERY, ~100 ms) plus its
                               * 2 ms query timeout, so the controller sees every change before its t0. */
#define SET_BUSY_PREFIX "busy:" /* start of a transient refusal (settings_schedule); clients may resend */

/* 1 si req es aceptable; si no, 0 y el motivo en why. cur son los ajustes vigentes. */
int settings_validate(const bg_settings_t *cur, const bg_settings_t *req, char *why, size_t n);

/* Tarea 3 del usuario: limite de cambio por comando (ver tareas/03_rate_limit). */
int settings_rate_ok(const bg_settings_t *cur, const bg_settings_t *req, char *why, size_t n);

/* Inicio de la primera respiracion que empieza al menos SET_LEAD_S despues de now, con el calendario de cur. */
double settings_next_breath(const bg_settings_t *cur, double now);

/* Start time for an accepted change requested at now. pending is the change already scheduled, or NULL.
 *   - No pending change: the first breath start at least SET_LEAD_S after now (settings_next_breath).
 *   - Pending change at least SET_LEAD_S away: the request replaces it at the same t0.
 *   - Pending change closer than SET_LEAD_S, or already due: refused (see step 4 above).
 * Returns 1 and writes *t0, or 0 with the reason in why, starting with SET_BUSY_PREFIX and without commas (it goes
 * to safety_mon's events CSV). */
int settings_schedule(const bg_settings_t *cur, const bg_settings_t *pending, double now, double *t0, char *why,
                      size_t n);

/* Numero de respiracion (desde t0 de cur) en now, y si esta en inspiracion. */
uint32_t settings_breath(const bg_settings_t *cur, double now, int *insp, double *into);

#endif
