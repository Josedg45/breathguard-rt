/*
 * latency_test.c - M1: mide el jitter de un lazo periódico en QNX.
 *
 * Un timer del kernel entrega un pulso cada periodo a un canal; un hilo
 * SCHED_FIFO lo recibe y marca el tiempo con el contador de ciclos.
 * Las muestras se guardan en RAM y el CSV se escribe AL FINAL, para que la
 * E/S a disco no contamine la medición.
 *
 * Definiciones (T = periodo nominal):
 *   period_us  = t[i] - t[i-1]
 *   jitter_us  = |period_us - T|
 *   lateness_us= t[i] - (t[0] + i*T)   desfase contra la rejilla ideal
 *   overrun    = period_us > 1.5*T     (se perdió el plazo de ese ciclo)
 */
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <time.h>
#include <unistd.h>

#define PULSE_CODE_TICK (_PULSE_CODE_MINAVAIL + 0)

static void die(const char *what)
{
    fprintf(stderr, "latency_test: %s: %s\n", what, strerror(errno));
    exit(EXIT_FAILURE);
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Percentil por rango más cercano sobre un arreglo ya ordenado. */
static double percentile(const double *sorted, size_t n, double p)
{
    size_t rank = (size_t)(p / 100.0 * (double)n + 0.999999);
    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    return sorted[rank - 1];
}

static void usage(void)
{
    fprintf(stderr,
        "uso: latency_test [-s segundos] [-f hz] [-p prioridad] [-C cpu]\n"
        "                  [-l etiqueta] [-c muestras.csv] [-S resumen.csv]\n"
        "  -C  ancla el hilo a esa CPU (por defecto: cualquiera)\n");
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    int seconds = 10, hz = 1000, prio = 50, cpu = -1, opt;
    const char *label = "run", *csv_path = NULL, *summary_path = NULL;

    while ((opt = getopt(argc, argv, "s:f:p:C:l:c:S:")) != -1) {
        switch (opt) {
        case 's': seconds = atoi(optarg); break;
        case 'f': hz = atoi(optarg); break;
        case 'p': prio = atoi(optarg); break;
        case 'C': cpu = atoi(optarg); break;
        case 'l': label = optarg; break;
        case 'c': csv_path = optarg; break;
        case 'S': summary_path = optarg; break;
        default: usage();
        }
    }
    if (seconds <= 0 || hz <= 0 || prio <= 0 || cpu >= _syspage_ptr->num_cpu) usage();

    size_t n = (size_t)seconds * (size_t)hz;
    const uint64_t period_ns = 1000000000ULL / (uint64_t)hz;
    const double period_us = (double)period_ns / 1000.0;

    /* Fijamos toda la memoria en RAM: un fallo de página en el lazo sería jitter. */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) die("mlockall");

    uint64_t *stamps = calloc(n + 1, sizeof *stamps);
    double *jit_sorted = calloc(n, sizeof *jit_sorted);
    double *per_sorted = calloc(n, sizeof *per_sorted);
    if (!stamps || !jit_sorted || !per_sorted) die("calloc");

    /* Tiempo real: SCHED_FIFO corre hasta bloquearse o ser desplazado por
     * alguien de mayor prioridad. */
    struct sched_param sp = { .sched_priority = prio };
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != 0) { errno = rc; die("pthread_setschedparam"); }

    /* Anclar a una CPU evita migraciones e IPIs entre vCPUs, caras bajo virtualización. */
    if (cpu >= 0 && ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)(1u << cpu)) == -1)
        die("ThreadCtl(RUNMASK)");

    /* Solo lectura: en QNX 8.0 el tick no se puede cambiar en ejecución (ENOTSUP). */
    struct _clockperiod cur_cp;
    if (ClockPeriod(CLOCK_REALTIME, NULL, &cur_cp, 0) == -1) die("ClockPeriod(get)");

    /* Canal privado + conexión a sí mismo: destino de los pulsos del timer. */
    int chid = ChannelCreate(_NTO_CHF_PRIVATE);
    if (chid == -1) die("ChannelCreate");
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) die("ConnectAttach");

    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, prio, PULSE_CODE_TICK, 0);

    timer_t timer;
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1) die("timer_create");

    struct itimerspec its;
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = (long)period_ns;
    its.it_interval = its.it_value;

    const uint64_t cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;

    if (timer_settime(timer, 0, &its, NULL) == -1) die("timer_settime");

    /* Lazo de medición: sin printf ni E/S. Hacen falta n+1 marcas para n periodos. */
    struct _pulse pulse;
    for (size_t i = 0; i <= n; i++) {
        if (MsgReceivePulse(chid, &pulse, sizeof pulse, NULL) == -1) die("MsgReceivePulse");
        stamps[i] = ClockCycles();
    }

    timer_delete(timer);
    /* Ya no medimos: bajamos la prioridad para no monopolizar la CPU con la E/S. */
    sp.sched_priority = 10;
    pthread_setschedparam(pthread_self(), SCHED_RR, &sp);

    /* Estadísticas */
    FILE *csv = NULL;
    if (csv_path) {
        csv = fopen(csv_path, "w");
        if (!csv) die(csv_path);
        fprintf(csv, "i,t_us,period_us,jitter_us,lateness_us\n");
    }

    double sum_period = 0, sum_jitter = 0;
    size_t overruns = 0;
    for (size_t i = 0; i < n; i++) {
        double p = (double)(stamps[i + 1] - stamps[i]) * 1e6 / (double)cps;
        double j = p > period_us ? p - period_us : period_us - p;
        per_sorted[i] = p;
        jit_sorted[i] = j;
        sum_period += p;
        sum_jitter += j;
        if (p > 1.5 * period_us) overruns++;
        if (csv) {
            double t = (double)(stamps[i + 1] - stamps[0]) * 1e6 / (double)cps;
            double late = t - (double)(i + 1) * period_us;
            fprintf(csv, "%zu,%.3f,%.3f,%.3f,%.3f\n", i, t, p, j, late);
        }
    }
    if (csv) fclose(csv);

    qsort(per_sorted, n, sizeof(double), cmp_double);
    qsort(jit_sorted, n, sizeof(double), cmp_double);

    double per_avg = sum_period / (double)n, jit_avg = sum_jitter / (double)n;
    double jit_p99 = percentile(jit_sorted, n, 99.0);
    double jit_p999 = percentile(jit_sorted, n, 99.9);

    printf("[%s] prio=%d cpu=%d T=%.0fus tick=%luns cps=%" PRIu64 " muestras=%zu\n",
           label, prio, cpu, period_us, (unsigned long)cur_cp.nsec, cps, n);
    printf("  periodo (us): min=%.3f max=%.3f prom=%.3f\n",
           per_sorted[0], per_sorted[n - 1], per_avg);
    printf("  jitter  (us): min=%.3f max=%.3f prom=%.3f p99=%.3f p99.9=%.3f\n",
           jit_sorted[0], jit_sorted[n - 1], jit_avg, jit_p99, jit_p999);
    printf("  overruns (periodo > 1.5T): %zu\n", overruns);

    if (summary_path) {
        int is_new = access(summary_path, F_OK) != 0;
        FILE *s = fopen(summary_path, "a");
        if (!s) die(summary_path);
        if (is_new)
            fprintf(s, "label,prio,cpu,period_us,tick_ns,samples,period_min_us,period_max_us,"
                       "period_avg_us,jitter_min_us,jitter_max_us,jitter_avg_us,"
                       "jitter_p99_us,jitter_p999_us,overruns\n");
        fprintf(s, "%s,%d,%d,%.0f,%lu,%zu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%zu\n",
                label, prio, cpu, period_us, (unsigned long)cur_cp.nsec, n,
                per_sorted[0], per_sorted[n - 1], per_avg,
                jit_sorted[0], jit_sorted[n - 1], jit_avg, jit_p99, jit_p999, overruns);
        fclose(s);
    }

    free(stamps);
    free(jit_sorted);
    free(per_sorted);
    return EXIT_SUCCESS;
}
