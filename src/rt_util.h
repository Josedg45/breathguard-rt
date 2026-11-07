/* rt_util.h - utilidades de tiempo real comunes a los procesos QNX de BreathGuard RT. */
#ifndef RT_UTIL_H
#define RT_UTIL_H

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <time.h>

static inline void rt_die(const char *prog, const char *what)
{
    fprintf(stderr, "%s: %s: %s\n", prog, what, strerror(errno));
    exit(EXIT_FAILURE);
}

/* Segundos desde el arranque, con el contador de ciclos de la CPU (ClockCycles, de QNX). */
static inline double rt_now_s(void)
{
    static double inv_cps = 0;
    if (inv_cps == 0) inv_cps = 1.0 / (double)SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    return (double)ClockCycles() * inv_cps;
}

/* Bloquea la memoria en RAM y pasa el hilo actual a SCHED_FIFO con esa prioridad. */
static inline void rt_setup(const char *prog, int prio)
{
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) rt_die(prog, "mlockall");
    struct sched_param sp = { .sched_priority = prio };
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != 0) { errno = rc; rt_die(prog, "pthread_setschedparam"); }
}

/* Timer periódico del kernel que manda un pulso con 'code' al canal chid, con prioridad prio. */
static inline timer_t rt_periodic_pulse(const char *prog, int chid, int prio, int code, long period_ns)
{
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) rt_die(prog, "ConnectAttach");
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, prio, code, 0);
    timer_t tm;
    if (timer_create(CLOCK_MONOTONIC, &ev, &tm) == -1) rt_die(prog, "timer_create");
    struct itimerspec its = { .it_value = { 0, period_ns }, .it_interval = { 0, period_ns } };
    if (timer_settime(tm, 0, &its, NULL) == -1) rt_die(prog, "timer_settime");
    return tm;
}

/* Vuelve a una prioridad normal para escribir resultados sin estorbar. */
static inline void rt_relax(void)
{
    struct sched_param sp = { .sched_priority = 10 };
    pthread_setschedparam(pthread_self(), SCHED_RR, &sp);
}

#endif
