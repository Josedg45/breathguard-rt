/* watchdog.c - safety_mon's heartbeat watchdog. Rules in watchdog.h; test in tests/test_watchdog.c. */
#include "watchdog.h"

/* Did MORE than limit_ns pass since since_ns? With uint64_t, now - since would wrap to a huge number if
 * now < since (rule 5), so that case is excluded first. Exactly at the limit has not expired (rule 3). */
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
    wd->last_seq = 0;          /* meaningless while state == WD_WAITING */
    wd->state = WD_WAITING;
}

void wd_beat(watchdog_t *wd, uint64_t now_ns, uint32_t seq)
{
    /* Rule 4: a beat that arrives after the deadline cannot erase the failure, even if nobody has called
     * wd_check yet. That is why the deadline is evaluated BEFORE accepting the beat. */
    if (wd_check(wd, now_ns) == WD_TIMEOUT)
        return;

    if (wd->state == WD_OK && seq == wd->last_seq)
        return;                /* rule 2: repeated sequence = the controller did not advance */

    /* The first beat is valid with any sequence (0 too): the state, not last_seq, says whether one arrived. */
    wd->last_seq = seq;
    wd->last_beat_ns = now_ns;
    wd->state = WD_OK;
}

wd_state_t wd_check(watchdog_t *wd, uint64_t now_ns)
{
    if (wd->state == WD_WAITING && expired(now_ns, wd->armed_at_ns, wd->startup_ns))
        wd->state = WD_TIMEOUT;   /* rule 1: never started */
    else if (wd->state == WD_OK && expired(now_ns, wd->last_beat_ns, wd->timeout_ns))
        wd->state = WD_TIMEOUT;   /* rule 3: stopped beating */
    return wd->state;              /* WD_TIMEOUT is latched (rule 4) */
}
