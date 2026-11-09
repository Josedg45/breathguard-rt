/* copilot_logic.h - the copilot's fixed rules and safety_mon's validation of its suggestions. Plain C, no QNX.
 *
 * What is LEARNED and what is FIXED (keep this distinction in every document):
 *   learned  the decision tree (src/copilot_tree.h, trained offline by scripts/copilot_train.py). It only answers
 *            "which pattern does this breath look like": normal, leak, high resistance or low compliance.
 *   fixed    everything below: the table that turns a class into an action (cp_rules, run by the copilot), and the
 *            checks safety_mon applies before the operator even sees it (cp_check). These are hand-written rules,
 *            reviewed like any other safety code. They are not AI and must not be called AI.
 *
 * Flow: safety_mon measures each breath (breath_feat) -> copilot: tree + cp_rules -> safety_mon: cp_check with its
 * OWN measurements -> operator accepts or rejects (cp_ack) -> only then a settings change goes through the normal,
 * validated settings path. The copilot never talks to the plant and never changes a setting by itself. */
#ifndef COPILOT_LOGIC_H
#define COPILOT_LOGIC_H

#include <stddef.h>
#include <stdint.h>

#include "breath_feat.h"
#include "settings.h"

enum { CP_NORMAL = 0, CP_LEAK, CP_HIGH_R, CP_LOW_C, CP_N_CLASSES };

enum {
    CP_ACT_NONE = 0,
    CP_ACT_CHECK_CIRCUIT,     /* leak: check cuff, mask and circuit. No settings change: more pressure feeds a leak */
    CP_ACT_CHECK_AIRWAY,      /* high resistance: check the tube (kink, secretions), consider bronchodilator */
    CP_ACT_LOWER_RR,          /* high resistance + measured auto-PEEP: RR - 2 (longer expiration) */
    CP_ACT_REVIEW_COMPLIANCE, /* low compliance: review the patient (no automatic change proposed) */
    CP_ACT_RAISE_PINSP,       /* low compliance + measured Vt < 6 mL/kg in PCV: Pinsp + 2, within limits */
    CP_N_ACTIONS
};

enum { CP_ST_NONE = 0, CP_ST_PENDING, CP_ST_ACCEPTED, CP_ST_REJECTED, CP_ST_EXPIRED, CP_ST_CLEARED, CP_N_STATES };

/* Fixed limits (rules, not learned). */
#define CP_CONF_MIN 0.80      /* below this tree confidence the copilot says nothing */
#define CP_HIST 5             /* persistence: the same class in >= CP_PERSIST of the last CP_HIST breaths */
#define CP_PERSIST 3
#define CP_EXPIRE_S 60.0      /* a proposal nobody answers expires */
#define CP_VT_LOW_ML_KG 6.0   /* below this Vt (per kg of ideal body weight) a pressure increase may be proposed */
#define CP_DRIVE_MAX 15.0     /* driving pressure (Pinsp - PEEP) never above this (lung-protective ventilation) */
#define CP_PINSP_MAX 30.0     /* Pinsp never above this (plateau pressure limit) */
#define CP_STEP_PINSP 2.0     /* the only step the copilot may propose */
#define CP_STEP_RR 2.0
#define CP_RR_MIN 10.0
#define CP_AUTOPEEP_MIN 2.0   /* cmH2O of auto-PEEP measured by safety_mon before proposing a lower RR */

/* One closed breath, as safety_mon measured it. safety_mon sends it to the copilot and keeps its own copy. */
typedef struct {
    uint32_t breath;      /* safety_mon's breath counter */
    int32_t valid;        /* 1 = f[] usable: breath_feat valid and no alarm, backup, relief or settings change in it */
    double f[BF_N];
    bg_settings_t set;    /* settings in force during the breath */
    double vti_ml, vte_ml;
    double ibw_kg;
    double auto_peep;     /* lung_mon estimate (cmH2O), -1 if unknown */
} cp_breath_t;

/* What the copilot proposes for one breath. */
typedef struct {
    uint32_t breath;
    int32_t cls;          /* -1 = no prediction (tree_predict said "don't know") */
    float conf;
    int32_t action;
    bg_settings_t req;    /* proposed settings (equal to the breath's settings if the action changes nothing) */
    uint32_t infer_ns;    /* time spent by the tree and the rules in the copilot */
} cp_suggest_t;

/* The copilot's table: class -> action and proposed settings. */
void cp_rules(const cp_breath_t *b, int cls, double conf, cp_suggest_t *out);

/* safety_mon's state about the copilot. */
typedef struct {
    int32_t hist[CP_HIST];   /* last classes with conf >= CP_CONF_MIN (-1 otherwise), newest at (n-1) % CP_HIST */
    uint32_t n;
    uint32_t last_breath;
    /* the current proposal */
    uint32_t id;             /* increases with every new proposal */
    int32_t state, cls, action;
    float conf;
    double t;                /* when it was proposed (or answered) */
    uint32_t base_seq;       /* settings seq it was built on: if the settings change, it no longer applies */
    bg_settings_t req;
    char text[64];           /* the proposal, human-readable (what the operator reads) */
    char why[64];            /* reason of the last blocked suggestion */
    uint32_t n_suggest, n_blocked, n_proposed, n_accepted, n_rejected, n_expired;
} cp_check_t;

void cp_check_init(cp_check_t *c);

/* safety_mon: a suggestion arrived. mine = safety_mon's own record of that breath (NULL if it has none); cur = the
 * settings in force; settings_pending = an operator change is already scheduled.
 * Returns 1 if a new proposal is now pending for the operator, 0 if nothing changed, -1 if the suggestion was
 * blocked (reason in why). The copilot's word is only trusted for the class: every premise (Vt, auto-PEEP, mode,
 * limits) is checked against safety_mon's own measurement. */
int cp_check(cp_check_t *c, const cp_breath_t *mine, const cp_suggest_t *s, const bg_settings_t *cur,
             int settings_pending, double now, char *why, size_t n);

/* safety_mon, every tick or so: expire an unanswered proposal. Returns 1 if it just expired. */
int cp_expire(cp_check_t *c, double now);

/* The operator answers proposal id (accept = 1 or 0). Returns 1 if the answer applies, with *req_out set to the
 * settings to schedule (only when *has_req). 0 otherwise, reason in why (wrong id, no longer pending, settings
 * changed since). */
int cp_ack(cp_check_t *c, uint32_t id, int accept, const bg_settings_t *cur, double now, int *has_req,
           bg_settings_t *req_out, char *why, size_t n);

const char *cp_class_name(int cls);
const char *cp_action_name(int action);
const char *cp_state_name(int state);

#endif
