/* test_watchdog.c - unit test of src/watchdog.c (rules in watchdog.h). Uses a simulated clock. */
#include <stdio.h>

#include "watchdog.h"

#define MS 1000000ULL
#define TIMEOUT (50 * MS)
#define STARTUP (500 * MS)
#define T0 (1000 * MS)

static int failures;

static const char *name(wd_state_t s)
{
    return s == WD_WAITING ? "WD_WAITING" : s == WD_OK ? "WD_OK" : s == WD_TIMEOUT ? "WD_TIMEOUT" : "?";
}

static void expect(const char *title, wd_state_t got, wd_state_t want, const char *rule)
{
    if (got == want) {
        printf("  OK    %s\n", title);
    } else {
        printf("  FAIL  %s\n        got %s, expected %s (%s)\n", title, name(got), name(want), rule);
        failures++;
    }
}

int main(void)
{
    watchdog_t wd;

    puts("Startup");
    wd_arm(&wd, T0, TIMEOUT, STARTUP);
    expect("freshly armed: waiting", wd_check(&wd, T0), WD_WAITING, "rule 1");
    expect("no beat, exactly at the startup limit: still waiting",
           wd_check(&wd, T0 + STARTUP), WD_WAITING, "rule 1");
    expect("no beat, past the startup limit: timeout", wd_check(&wd, T0 + STARTUP + 1), WD_TIMEOUT, "rule 1");

    puts("Normal beats");
    wd_arm(&wd, T0, TIMEOUT, STARTUP);
    wd_beat(&wd, T0 + 10 * MS, 1);
    expect("the first beat moves to OK", wd_check(&wd, T0 + 10 * MS), WD_OK, "rule 2");
    /* A startup limit much longer than the timeout must not shield a controller that is already running. */
    expect("after startup the timeout rules, not the startup limit",
           wd_check(&wd, T0 + 10 * MS + TIMEOUT + 1), WD_TIMEOUT, "rule 3");

    wd_arm(&wd, T0, TIMEOUT, STARTUP);
    wd_beat(&wd, T0 + 10 * MS, 1);
    wd_beat(&wd, T0 + 40 * MS, 2);
    expect("exactly at the timeout: still OK", wd_check(&wd, T0 + 40 * MS + TIMEOUT), WD_OK, "rule 3");
    wd_beat(&wd, T0 + 40 * MS + TIMEOUT, 3);
    expect("each beat renews the deadline", wd_check(&wd, T0 + 40 * MS + TIMEOUT + TIMEOUT), WD_OK, "rule 3");

    puts("Hung controller");
    wd_arm(&wd, T0, TIMEOUT, STARTUP);
    wd_beat(&wd, T0 + 10 * MS, 7);
    wd_beat(&wd, T0 + 40 * MS, 7);
    expect("a repeated sequence does not renew the deadline", wd_check(&wd, T0 + 10 * MS + TIMEOUT + 1),
           WD_TIMEOUT, "rule 2");

    wd_arm(&wd, T0, TIMEOUT, STARTUP);
    wd_beat(&wd, T0 + 10 * MS, 0);
    expect("sequence 0 is valid as the first beat", wd_check(&wd, T0 + 10 * MS), WD_OK, "rule 2");

    puts("A failure does not heal by itself");
    wd_arm(&wd, T0, TIMEOUT, STARTUP);
    wd_beat(&wd, T0 + 10 * MS, 1);
    wd_check(&wd, T0 + 10 * MS + TIMEOUT + 1);
    wd_beat(&wd, T0 + 10 * MS + TIMEOUT + 2, 2);
    expect("a late beat does not revive the watchdog", wd_check(&wd, T0 + 10 * MS + TIMEOUT + 2), WD_TIMEOUT,
           "rule 4");
    wd_arm(&wd, T0 + 2000 * MS, TIMEOUT, STARTUP);
    expect("re-arming resets it", wd_check(&wd, T0 + 2000 * MS), WD_WAITING, "rule 4");

    puts("Out-of-order times");
    wd_arm(&wd, T0, TIMEOUT, STARTUP);
    wd_beat(&wd, T0 + 100 * MS, 1);
    expect("a beat newer than now is not a timeout", wd_check(&wd, T0 + 99 * MS), WD_OK, "rule 5");

    if (failures) {
        printf("\nRESULT: %d check(s) failed\n", failures);
        return 1;
    }
    puts("\nRESULT: all checks passed");
    return 0;
}
