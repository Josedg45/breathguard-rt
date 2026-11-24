/* test_rate_limit.c - unit test of src/rate_limit.c (maximum change per operator command). */
#include <stdio.h>
#include <string.h>

#include "settings.h"

static int fails;
static void check(const char *name, const bg_settings_t *cur, const bg_settings_t *req, int want)
{
    char why[64] = "";
    int got = settings_rate_ok(cur, req, why, sizeof why);
    int why_ok = want || (why[0] && strcmp(why, "ok") != 0);
    printf("%-44s %s  (expects %s, got %s%s%s)\n", name, got == want && why_ok ? "OK  " : "FAIL",
           want ? "accept" : "reject", got ? "accept" : "reject", got ? "" : ": ", got ? "" : why);
    if (got != want || !why_ok) fails++;
}

int main(void)
{
    bg_settings_t pcv = { .vcv = 0, .set = { .rr = 12, .ie = 2, .p_insp = 20, .peep = 5 }, .vt = 0.5 };
    bg_settings_t vcv = pcv;
    vcv.vcv = 1;
    bg_settings_t r;

    r = pcv; check("no change", &pcv, &r, 1);
    r = pcv; r.set.p_insp = 25; check("p_insp +5 (exactly at the limit)", &pcv, &r, 1);
    r = pcv; r.set.p_insp = 25.5; check("p_insp +5.5", &pcv, &r, 0);
    r = pcv; r.set.p_insp = 14; check("p_insp -6", &pcv, &r, 0);
    r = pcv; r.set.peep = 8; check("PEEP +3", &pcv, &r, 1);
    r = pcv; r.set.peep = 1; check("PEEP -4", &pcv, &r, 0);
    r = pcv; r.set.rr = 16; check("rr +4", &pcv, &r, 1);
    r = pcv; r.set.rr = 6; check("rr -6", &pcv, &r, 0);
    r = pcv; r.set.ie = 4; check("I:E 1:2 -> 1:4 (no limit)", &pcv, &r, 1);
    r = vcv; r.vt = 0.6; check("VCV Vt +100 mL", &vcv, &r, 1);
    r = vcv; r.vt = 0.35; check("VCV Vt -150 mL", &vcv, &r, 0);
    r = vcv; r.set.p_insp = 35; check("VCV: p_insp does not count", &vcv, &r, 1);
    r = pcv; r.vcv = 1; r.vt = 0.3; check("PCV -> VCV (mode change)", &pcv, &r, 1);
    r = pcv; r.vcv = 1; r.set.peep = 10; check("mode change with PEEP +5", &pcv, &r, 0);
    printf(fails ? "\nRESULT: %d check(s) failed\n" : "\nRESULT: all checks passed\n", fails);
    return fails != 0;
}
