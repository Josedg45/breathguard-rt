/* copilot_logic.c - fixed rules of the copilot and safety_mon's checks. See copilot_logic.h. */
#include "copilot_logic.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *const cls_names[CP_N_CLASSES] = { "normal", "leak", "high_resistance", "low_compliance" };
static const char *const act_names[CP_N_ACTIONS] = { "none", "check_circuit", "check_airway", "lower_rr",
                                                     "review_compliance", "raise_pinsp" };
static const char *const st_names[CP_N_STATES] = { "none", "pending", "accepted", "rejected", "expired", "cleared" };

const char *cp_class_name(int c) { return c >= 0 && c < CP_N_CLASSES ? cls_names[c] : "unknown"; }
const char *cp_action_name(int a) { return a >= 0 && a < CP_N_ACTIONS ? act_names[a] : "invalid"; }
const char *cp_state_name(int s) { return s >= 0 && s < CP_N_STATES ? st_names[s] : "invalid"; }

static int changes_settings(int action) { return action == CP_ACT_LOWER_RR || action == CP_ACT_RAISE_PINSP; }

/* ---------------- The copilot's table (fixed rules) ---------------- */
void cp_rules(const cp_breath_t *b, int cls, double conf, cp_suggest_t *o)
{
    memset(o, 0, sizeof *o);
    o->breath = b->breath;
    o->cls = cls;
    o->conf = (float)conf;
    o->req = b->set;
    o->action = CP_ACT_NONE;
    if (cls < 0 || !(conf >= CP_CONF_MIN)) return;
    switch (cls) {
    case CP_LEAK:
        o->action = CP_ACT_CHECK_CIRCUIT;
        break;
    case CP_HIGH_R:
        if (b->auto_peep >= CP_AUTOPEEP_MIN && b->set.set.rr - CP_STEP_RR >= CP_RR_MIN) {
            o->action = CP_ACT_LOWER_RR;
            o->req.set.rr = b->set.set.rr - CP_STEP_RR;
        } else {
            o->action = CP_ACT_CHECK_AIRWAY;
        }
        break;
    case CP_LOW_C: {
        double p = b->set.set.p_insp + CP_STEP_PINSP;
        if (!b->set.vcv && b->ibw_kg > 0 && b->vti_ml / b->ibw_kg < CP_VT_LOW_ML_KG &&
            p - b->set.set.peep <= CP_DRIVE_MAX && p <= CP_PINSP_MAX) {
            o->action = CP_ACT_RAISE_PINSP;
            o->req.set.p_insp = p;
        } else {
            o->action = CP_ACT_REVIEW_COMPLIANCE;
        }
        break;
    }
    default:
        break;
    }
}

/* ---------------- safety_mon's checks ---------------- */
void cp_check_init(cp_check_t *c)
{
    memset(c, 0, sizeof *c);
    for (int i = 0; i < CP_HIST; i++) c->hist[i] = -1;
}

static int votes(const cp_check_t *c, int cls)
{
    int k = 0;
    for (int i = 0; i < CP_HIST; i++) k += c->hist[i] == cls;
    return k;
}

static int allowed(int cls, int action)
{
    switch (cls) {
    case CP_LEAK: return action == CP_ACT_CHECK_CIRCUIT;
    case CP_HIGH_R: return action == CP_ACT_CHECK_AIRWAY || action == CP_ACT_LOWER_RR;
    case CP_LOW_C: return action == CP_ACT_REVIEW_COMPLIANCE || action == CP_ACT_RAISE_PINSP;
    default: return 0;
    }
}

static int same_settings(const bg_settings_t *a, const bg_settings_t *b)
{
    return a->vcv == b->vcv && fabs(a->set.p_insp - b->set.p_insp) < 1e-6 && fabs(a->set.peep - b->set.peep) < 1e-6 &&
           fabs(a->set.rr - b->set.rr) < 1e-6 && fabs(a->set.ie - b->set.ie) < 1e-6 && fabs(a->vt - b->vt) < 1e-9;
}

static int block(cp_check_t *c, char *why, size_t n, const char *msg)
{
    snprintf(why, n, "%s", msg);
    snprintf(c->why, sizeof c->why, "%s", msg);
    c->n_blocked++;
    return -1;
}

int cp_check(cp_check_t *c, const cp_breath_t *mine, const cp_suggest_t *s, const bg_settings_t *cur,
             int settings_pending, double now, char *why, size_t n)
{
    char msg[64];
    c->n_suggest++;
    snprintf(why, n, "ok");
    if (!mine || mine->breath != s->breath) {
        snprintf(msg, sizeof msg, "no record of breath %u", s->breath);
        return block(c, why, n, msg);
    }
    if (c->n && s->breath <= c->last_breath) { /* one answer per breath: a repeated or old breath is a replay */
        snprintf(msg, sizeof msg, "breath %u already answered", s->breath);
        return block(c, why, n, msg);
    }
    if (s->cls < -1 || s->cls >= CP_N_CLASSES || s->action < 0 || s->action >= CP_N_ACTIONS ||
        !(s->conf >= 0 && s->conf <= 1))
        return block(c, why, n, "class, action or confidence out of range");
    c->last_breath = s->breath;
    int vote = mine->valid && s->cls >= 0 && s->conf >= CP_CONF_MIN ? s->cls : -1;
    c->hist[c->n++ % CP_HIST] = vote;

    if (!mine->valid) return s->action == CP_ACT_NONE ? 0 : block(c, why, n, "breath not valid for the copilot");

    if (c->state == CP_ST_PENDING) {
        if (votes(c, CP_NORMAL) >= CP_PERSIST) { /* the condition went away (e.g. the circuit was fixed) */
            c->state = CP_ST_CLEARED;
            c->t = now;
            return 0;
        }
        if (s->cls == c->cls) return 0; /* already proposed */
    }
    if (s->action == CP_ACT_NONE || votes(c, s->cls) < CP_PERSIST) return 0; /* nothing, or not persistent yet */
    if (!allowed(s->cls, s->action)) {
        snprintf(msg, sizeof msg, "action %s not allowed for %s", cp_action_name(s->action), cp_class_name(s->cls));
        return block(c, why, n, msg);
    }

    bg_settings_t want = *cur;
    if (changes_settings(s->action)) {
        if (settings_pending || mine->set.seq != cur->seq) return 0; /* settings moving: wait for a steady breath */
        if (s->action == CP_ACT_RAISE_PINSP) {
            double vtkg = mine->ibw_kg > 0 ? mine->vti_ml / mine->ibw_kg : NAN;
            if (cur->vcv) return block(c, why, n, "Pinsp change proposed in VCV");
            if (!(vtkg < CP_VT_LOW_ML_KG)) {
                snprintf(msg, sizeof msg, "measured Vt %.1f mL/kg is not below %.0f", vtkg, CP_VT_LOW_ML_KG);
                return block(c, why, n, msg);
            }
            want.set.p_insp = cur->set.p_insp + CP_STEP_PINSP;
            if (want.set.p_insp - want.set.peep > CP_DRIVE_MAX + 1e-9 || want.set.p_insp > CP_PINSP_MAX + 1e-9) {
                snprintf(msg, sizeof msg, "Pinsp %.1f / driving %.1f over the limits", want.set.p_insp,
                         want.set.p_insp - want.set.peep);
                return block(c, why, n, msg);
            }
        } else {
            if (!(mine->auto_peep >= CP_AUTOPEEP_MIN)) {
                snprintf(msg, sizeof msg, "measured auto-PEEP %.1f is not >= %.0f", mine->auto_peep, CP_AUTOPEEP_MIN);
                return block(c, why, n, msg);
            }
            want.set.rr = cur->set.rr - CP_STEP_RR;
            if (want.set.rr < CP_RR_MIN - 1e-9) return block(c, why, n, "RR would go below the minimum");
        }
        if (!same_settings(&s->req, &want)) return block(c, why, n, "proposed settings differ from the allowed step");
        char w2[44];
        if (!settings_validate(cur, &want, w2, sizeof w2)) {
            snprintf(msg, sizeof msg, "settings_validate: %s", w2);
            return block(c, why, n, msg);
        }
    } else if (!same_settings(&s->req, &mine->set)) {
        return block(c, why, n, "advice-only action carries a settings change");
    }

    c->id++;
    c->state = CP_ST_PENDING;
    c->cls = s->cls;
    c->action = s->action;
    c->conf = s->conf;
    c->t = now;
    c->base_seq = cur->seq;
    c->req = want;
    c->n_proposed++;
    if (s->action == CP_ACT_RAISE_PINSP)
        snprintf(c->text, sizeof c->text, "Pinsp %.1f -> %.1f cmH2O", cur->set.p_insp, want.set.p_insp);
    else if (s->action == CP_ACT_LOWER_RR)
        snprintf(c->text, sizeof c->text, "RR %.1f -> %.1f /min", cur->set.rr, want.set.rr);
    else
        snprintf(c->text, sizeof c->text, "%s (no settings change)", cp_action_name(s->action));
    snprintf(why, n, "%s", c->text);
    return 1;
}

int cp_expire(cp_check_t *c, double now)
{
    if (c->state != CP_ST_PENDING || now - c->t < CP_EXPIRE_S) return 0;
    c->state = CP_ST_EXPIRED;
    c->t = now;
    c->n_expired++;
    return 1;
}

int cp_ack(cp_check_t *c, uint32_t id, int accept, const bg_settings_t *cur, double now, int *has_req,
           bg_settings_t *req_out, char *why, size_t n)
{
    *has_req = 0;
    if (id != c->id || c->state != CP_ST_PENDING) {
        snprintf(why, n, "proposal %u is not pending (current %u, %s)", id, c->id, cp_state_name(c->state));
        return 0;
    }
    c->t = now;
    if (!accept) {
        c->state = CP_ST_REJECTED;
        c->n_rejected++;
        snprintf(why, n, "rejected by the operator");
        return 1;
    }
    if (changes_settings(c->action)) {
        if (cur->seq != c->base_seq) {
            c->state = CP_ST_EXPIRED;
            c->n_expired++;
            snprintf(why, n, "settings changed since the proposal");
            return 0;
        }
        *has_req = 1;
        *req_out = c->req;
    }
    c->state = CP_ST_ACCEPTED;
    c->n_accepted++;
    snprintf(why, n, "accepted: %s", c->text);
    return 1;
}
