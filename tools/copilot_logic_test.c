/* copilot_logic_test.c - host test of the copilot's fixed rules and of safety_mon's checks (copilot_logic.c).
 * An honest copilot and a lying one: everything the copilot may get wrong must be blocked by safety_mon.
 * Build and run: make copilot-logic-test */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "copilot_logic.h"

static int fails;
#define CHECK(name, cond) do { int ok_ = (cond); printf("%-66s %s\n", name, ok_ ? "OK" : "FAIL"); fails += !ok_; } while (0)

static bg_settings_t pcv(void)
{
    bg_settings_t s = { .seq = 0, .vcv = 0, .set = { .rr = 12, .ie = 2, .p_insp = 18, .peep = 5, .rise_s = 0.1 },
                        .vt = 0.5 };
    return s;
}

static cp_breath_t breath(uint32_t k, const bg_settings_t *set, double vti_ml, double auto_peep)
{
    cp_breath_t b = { .breath = k, .valid = 1, .set = *set, .vti_ml = vti_ml, .vte_ml = vti_ml, .ibw_kg = 75,
                      .auto_peep = auto_peep };
    return b;
}

/* Feeds n breaths of class cls (honest copilot) and returns the last cp_check result. */
static int feed(cp_check_t *c, uint32_t *k, int n, int cls, const bg_settings_t *cur, double vti, double ap, double t)
{
    int r = 0;
    char why[96];
    for (int i = 0; i < n; i++) {
        cp_breath_t b = breath(++*k, cur, vti, ap);
        cp_suggest_t s;
        cp_rules(&b, cls, 0.95, &s);
        r = cp_check(c, &b, &s, cur, 0, t, why, sizeof why);
    }
    return r;
}

int main(void)
{
    bg_settings_t cur = pcv();
    cp_breath_t b;
    cp_suggest_t s;
    char why[96];
    double vtkg_low = 5.0 * 75, vtkg_ok = 7.0 * 75; /* mL */

    /* --- the table --- */
    b = breath(1, &cur, vtkg_low, 0);
    cp_rules(&b, CP_NORMAL, 0.99, &s);   CHECK("rules: normal -> none", s.action == CP_ACT_NONE);
    cp_rules(&b, CP_LEAK, 0.99, &s);     CHECK("rules: leak -> check circuit, no settings change",
                                               s.action == CP_ACT_CHECK_CIRCUIT && s.req.set.p_insp == 18);
    cp_rules(&b, CP_LOW_C, 0.99, &s);    CHECK("rules: low C + Vt 5 mL/kg in PCV 18/5 -> Pinsp +2",
                                               s.action == CP_ACT_RAISE_PINSP && fabs(s.req.set.p_insp - 20) < 1e-9);
    cp_rules(&b, CP_LOW_C, 0.70, &s);    CHECK("rules: confidence 0.70 -> none", s.action == CP_ACT_NONE);
    b = breath(1, &cur, vtkg_ok, 0);
    cp_rules(&b, CP_LOW_C, 0.99, &s);    CHECK("rules: low C but Vt 7 mL/kg -> review only",
                                               s.action == CP_ACT_REVIEW_COMPLIANCE);
    bg_settings_t hi = cur;
    hi.set.p_insp = 19; hi.set.peep = 5; /* 21 - 5 = 16 > 15 */
    b = breath(1, &hi, vtkg_low, 0);
    cp_rules(&b, CP_LOW_C, 0.99, &s);    CHECK("rules: Pinsp +2 would exceed driving 15 -> review only",
                                               s.action == CP_ACT_REVIEW_COMPLIANCE);
    b = breath(1, &cur, vtkg_ok, 3.0);
    cp_rules(&b, CP_HIGH_R, 0.99, &s);   CHECK("rules: high R + auto-PEEP 3 -> RR -2",
                                               s.action == CP_ACT_LOWER_RR && fabs(s.req.set.rr - 10) < 1e-9);
    b = breath(1, &cur, vtkg_ok, 0.5);
    cp_rules(&b, CP_HIGH_R, 0.99, &s);   CHECK("rules: high R without auto-PEEP -> check airway",
                                               s.action == CP_ACT_CHECK_AIRWAY);
    b.set.vcv = 1;
    cp_rules(&b, CP_LOW_C, 0.99, &s);    CHECK("rules: low C in VCV -> review only", s.action == CP_ACT_REVIEW_COMPLIANCE);

    /* --- persistence --- */
    cp_check_t c;
    uint32_t k = 0;
    cp_check_init(&c);
    int r = feed(&c, &k, 2, CP_LEAK, &cur, vtkg_ok, 0, 1.0);
    CHECK("check: 2 leak breaths -> nothing yet", r == 0 && c.state == CP_ST_NONE);
    r = feed(&c, &k, 1, CP_LEAK, &cur, vtkg_ok, 0, 1.0);
    CHECK("check: 3rd leak breath -> proposal pending", r == 1 && c.state == CP_ST_PENDING && c.id == 1);
    r = feed(&c, &k, 2, CP_LEAK, &cur, vtkg_ok, 0, 2.0);
    CHECK("check: more leak breaths -> same proposal, no new one", r == 0 && c.id == 1);
    feed(&c, &k, 3, CP_NORMAL, &cur, vtkg_ok, 0, 3.0);
    CHECK("check: 3 normal breaths -> proposal cleared", c.state == CP_ST_CLEARED);

    /* --- the operator --- */
    cp_check_init(&c);
    k = 0;
    feed(&c, &k, 3, CP_LOW_C, &cur, vtkg_low, 0, 10.0);
    CHECK("check: low C x3, safety_mon measured Vt 5 mL/kg -> Pinsp 20 pending",
          c.state == CP_ST_PENDING && fabs(c.req.set.p_insp - 20) < 1e-9);
    int has;
    bg_settings_t req;
    r = cp_ack(&c, c.id + 1, 1, &cur, 11.0, &has, &req, why, sizeof why);
    CHECK("ack: wrong id -> refused", r == 0 && c.state == CP_ST_PENDING);
    bg_settings_t moved = cur;
    moved.seq = 1;
    r = cp_ack(&c, c.id, 1, &moved, 11.0, &has, &req, why, sizeof why);
    CHECK("ack: settings changed since the proposal -> refused, expired", r == 0 && c.state == CP_ST_EXPIRED);
    cp_check_init(&c);
    k = 0;
    feed(&c, &k, 3, CP_LOW_C, &cur, vtkg_low, 0, 10.0);
    r = cp_ack(&c, c.id, 1, &cur, 11.0, &has, &req, why, sizeof why);
    CHECK("ack: accept -> settings to schedule: Pinsp 20", r == 1 && has && fabs(req.set.p_insp - 20) < 1e-9 &&
                                                            c.state == CP_ST_ACCEPTED);
    cp_check_init(&c);
    k = 0;
    feed(&c, &k, 3, CP_LEAK, &cur, vtkg_ok, 0, 10.0);
    r = cp_ack(&c, c.id, 0, &cur, 11.0, &has, &req, why, sizeof why);
    CHECK("ack: reject -> rejected, nothing to schedule", r == 1 && !has && c.state == CP_ST_REJECTED);
    cp_check_init(&c);
    k = 0;
    feed(&c, &k, 3, CP_LEAK, &cur, vtkg_ok, 0, 10.0);
    CHECK("expire: 59 s -> still pending", cp_expire(&c, 69.0) == 0 && c.state == CP_ST_PENDING);
    CHECK("expire: 61 s -> expired", cp_expire(&c, 71.0) == 1 && c.state == CP_ST_EXPIRED);
    bg_settings_t demo = cur;
    demo.set.p_insp = 20; /* the demo's PCV 20/5: driving pressure already 15 */
    cp_check_init(&c);
    k = 0;
    feed(&c, &k, 3, CP_LOW_C, &demo, vtkg_low, 0, 10.0);
    CHECK("check: low C at PCV 20/5 (driving 15) -> review only, no pressure increase",
          c.state == CP_ST_PENDING && c.action == CP_ACT_REVIEW_COMPLIANCE && fabs(c.req.set.p_insp - 20) < 1e-9);

    /* --- a lying copilot: safety_mon trusts only the class and checks the rest itself --- */
    struct { const char *name; int cls, action; double p_insp, peep, rr; double vti; double ap; int vcv; } lie[] = {
        { "lie: low C, Pinsp +2, but safety_mon measured Vt 7 mL/kg", CP_LOW_C, CP_ACT_RAISE_PINSP, 20, 5, 12,
          7.0 * 75, 0, 0 },
        { "lie: low C, Pinsp +6 (step too big)", CP_LOW_C, CP_ACT_RAISE_PINSP, 24, 5, 12, 5.0 * 75, 0, 0 },
        { "lie: low C, Pinsp +2 and PEEP +3 at the same time", CP_LOW_C, CP_ACT_RAISE_PINSP, 20, 8, 12, 5.0 * 75, 0,
          0 },
        { "lie: leak, but 'check circuit' carries Pinsp +2", CP_LEAK, CP_ACT_CHECK_CIRCUIT, 20, 5, 12, 7.0 * 75, 0, 0 },
        { "lie: leak with action 'raise Pinsp' (not allowed for leak)", CP_LEAK, CP_ACT_RAISE_PINSP, 20, 5, 12,
          5.0 * 75, 0, 0 },
        { "lie: high R, RR -2, but safety_mon measured auto-PEEP 0.5", CP_HIGH_R, CP_ACT_LOWER_RR, 18, 5, 10, 7.0 * 75,
          0.5, 0 },
        { "lie: low C, Pinsp +2 in VCV", CP_LOW_C, CP_ACT_RAISE_PINSP, 20, 5, 12, 5.0 * 75, 0, 1 },
    };
    for (unsigned i = 0; i < sizeof lie / sizeof lie[0]; i++) {
        cp_check_init(&c);
        bg_settings_t base = cur;
        base.vcv = lie[i].vcv;
        int blocked = 0;
        char bwhy[96] = "";
        for (uint32_t j = 1; j <= 3; j++) {
            b = breath(j, &base, lie[i].vti, lie[i].ap);
            memset(&s, 0, sizeof s);
            s.breath = j; s.cls = lie[i].cls; s.conf = 0.99f; s.action = lie[i].action; s.req = base;
            s.req.set.p_insp = lie[i].p_insp; s.req.set.peep = lie[i].peep; s.req.set.rr = lie[i].rr;
            if (cp_check(&c, &b, &s, &base, 0, 1.0, why, sizeof why) == -1 && !blocked) {
                blocked = 1;
                snprintf(bwhy, sizeof bwhy, "%s", why);
            }
        }
        char name[96];
        snprintf(name, sizeof name, "%s", lie[i].name);
        CHECK(name, blocked && c.state == CP_ST_NONE);
        printf("      blocked because: %s\n", bwhy);
    }
    cp_check_init(&c);
    b = breath(5, &cur, vtkg_ok, 0);
    cp_rules(&b, CP_LEAK, 0.95, &s);
    cp_check(&c, &b, &s, &cur, 0, 1.0, why, sizeof why);
    CHECK("lie: the same breath answered twice (replay) -> blocked", cp_check(&c, &b, &s, &cur, 0, 1, why, 96) == -1);
    s.breath = 6;
    CHECK("lie: a breath safety_mon has no record of -> blocked", cp_check(&c, &b, &s, &cur, 0, 1, why, 96) == -1);
    b = breath(7, &cur, vtkg_ok, 0);
    cp_rules(&b, CP_LEAK, 0.95, &s);
    s.conf = NAN;
    CHECK("lie: confidence NaN -> blocked", cp_check(&c, &b, &s, &cur, 0, 1, why, 96) == -1);
    b = breath(8, &cur, vtkg_ok, 0);
    cp_rules(&b, CP_LEAK, 0.95, &s);
    s.cls = 9;
    CHECK("lie: class 9 -> blocked", cp_check(&c, &b, &s, &cur, 0, 1, why, 96) == -1);
    b = breath(9, &cur, vtkg_ok, 0);
    b.valid = 0;
    cp_rules(&b, CP_LEAK, 0.95, &s);
    CHECK("lie: suggestion on a breath safety_mon marked invalid -> blocked",
          cp_check(&c, &b, &s, &cur, 0, 1, why, 96) == -1);

    printf("\ncopilot_logic_test: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
