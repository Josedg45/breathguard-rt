/* rate_limit.c - maximum change per operator command.
 *
 * safety_mon calls settings_rate_ok() from settings_validate() with the settings in force (cur) and the
 * requested ones (req), AFTER checking that req is inside the absolute limits. A valid value can still be
 * a dangerous jump (typing 35 instead of 15), so each command may move a setting only by a bounded step;
 * larger changes take several commands, one per breath. Returns 1 if the step is acceptable; otherwise 0
 * with the reason in why (n bytes), which the HMI shows to the operator.
 */
#include <math.h>
#include <stdio.h>

#include "settings.h"

#define RATE_P_INSP 5.0 /* cmH2O per command (PCV) */
#define RATE_VT 0.100   /* L per command (VCV) */
#define RATE_PEEP 3.0   /* cmH2O per command */
#define RATE_RR 4.0     /* breaths/min per command */
#define RATE_EPS 1e-9   /* a step equal to the limit is accepted despite rounding */

int settings_rate_ok(const bg_settings_t *cur, const bg_settings_t *req, char *why, size_t n)
{
    /* The inspiratory target is compared only within one mode: p_insp and Vt belong to different modes. */
    if (req->vcv == cur->vcv) {
        if (!req->vcv) {
            double d = fabs(req->set.p_insp - cur->set.p_insp);
            if (d > RATE_P_INSP + RATE_EPS) {
                snprintf(why, n, "p_insp changes %.1f > %.0f cmH2O per command", d, RATE_P_INSP);
                return 0;
            }
        } else {
            double d = fabs(req->vt - cur->vt);
            if (d > RATE_VT + RATE_EPS) {
                snprintf(why, n, "Vt changes %.0f > %.0f mL per command", d * 1000.0, RATE_VT * 1000.0);
                return 0;
            }
        }
    }
    double dp = fabs(req->set.peep - cur->set.peep);
    if (dp > RATE_PEEP + RATE_EPS) {
        snprintf(why, n, "PEEP changes %.1f > %.0f cmH2O per command", dp, RATE_PEEP);
        return 0;
    }
    double dr = fabs(req->set.rr - cur->set.rr);
    if (dr > RATE_RR + RATE_EPS) {
        snprintf(why, n, "rr changes %.1f > %.0f rpm per command", dr, RATE_RR);
        return 0;
    }
    return 1;
}
