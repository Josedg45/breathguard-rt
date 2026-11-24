/* watchdog.h - safety_mon's heartbeat watchdog for breath_ctrl. No QNX APIs: time is passed in as an
 * argument, so the logic is tested with a simulated clock (tests/test_watchdog.c).
 *
 * Rules:
 *   1. Startup: armed in WD_WAITING; if no valid beat arrives within startup_ns, the state becomes WD_TIMEOUT.
 *   2. A beat is valid if it is the first one (any sequence number, 0 included) or if its sequence number
 *      differs from the last valid one: a stuck thread can keep repeating the same beat.
 *   3. Once running, WD_TIMEOUT when MORE than timeout_ns pass since the last valid beat (exactly at the
 *      limit is still on time). Each valid beat renews the deadline.
 *   4. WD_TIMEOUT is latched: a late beat does not clear it, only wd_arm() does.
 *   5. Out-of-order times (a beat newer than now) are not a timeout: unsigned subtraction is guarded.
 */
#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WD_WAITING, /* armed, no valid beat yet */
    WD_OK,      /* receiving beats on time */
    WD_TIMEOUT  /* the controller failed: go to the safe state */
} wd_state_t;

typedef struct {
    uint64_t timeout_ns;   /* maximum time allowed between beats */
    uint64_t startup_ns;   /* maximum time allowed until the first beat */
    uint64_t armed_at_ns;  /* when the watchdog was armed */
    uint64_t last_beat_ns; /* when the last valid beat arrived */
    uint32_t last_seq;     /* sequence number of the last valid beat */
    wd_state_t state;
} watchdog_t;

/* Arms the watchdog in WD_WAITING. */
void wd_arm(watchdog_t *wd, uint64_t now_ns, uint64_t timeout_ns, uint64_t startup_ns);

/* Records a controller beat with its sequence number. */
void wd_beat(watchdog_t *wd, uint64_t now_ns, uint32_t seq);

/* Evaluates the elapsed time, updates the state and returns it. */
wd_state_t wd_check(watchdog_t *wd, uint64_t now_ns);

#endif
