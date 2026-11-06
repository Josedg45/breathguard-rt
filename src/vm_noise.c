/*
 * vm_noise.c - mide el "piso de ruido" de la máquina (detector de huecos).
 *
 * Un hilo anclado a una CPU, SCHED_FIFO a prioridad muy alta, gira leyendo el
 * contador de ciclos sin bloquearse nunca. Ningún hilo de QNX puede desplazarlo,
 * así que un hueco grande entre dos lecturas solo puede ser una interrupción
 * (microsegundos) o que el hipervisor le quitó la vCPU a la VM.
 * Separa el ruido del host del comportamiento real de QNX.
 */
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <unistd.h>

static const double EDGES_US[] = { 10, 50, 100, 500, 1000, 5000 };
#define N_EDGES (sizeof EDGES_US / sizeof EDGES_US[0])

int main(int argc, char **argv)
{
    int seconds = 5, cpu = 1, prio = 200, opt;
    const char *label = "noise", *summary_path = NULL;

    while ((opt = getopt(argc, argv, "s:c:p:l:S:")) != -1) {
        switch (opt) {
        case 's': seconds = atoi(optarg); break;
        case 'c': cpu = atoi(optarg); break;
        case 'p': prio = atoi(optarg); break;
        case 'l': label = optarg; break;
        case 'S': summary_path = optarg; break;
        default:
            fprintf(stderr, "uso: vm_noise [-s seg] [-c cpu] [-p prio] [-l etiqueta] [-S resumen.csv]\n");
            return EXIT_FAILURE;
        }
    }
    if (seconds <= 0 || cpu < 0 || cpu >= _syspage_ptr->num_cpu || prio <= 0) {
        fprintf(stderr, "vm_noise: argumentos inválidos\n");
        return EXIT_FAILURE;
    }

    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)(1u << cpu)) == -1) {
        perror("vm_noise: ThreadCtl(RUNMASK)");
        return EXIT_FAILURE;
    }
    struct sched_param sp = { .sched_priority = prio };
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != 0) {
        fprintf(stderr, "vm_noise: pthread_setschedparam: %s\n", strerror(rc));
        return EXIT_FAILURE;
    }

    const uint64_t cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    const double us_per_cycle = 1e6 / (double)cps;
    const uint64_t threshold = (uint64_t)(EDGES_US[0] / us_per_cycle);

    uint64_t over[N_EDGES] = { 0 };
    uint64_t max_gap = 0, lost = 0;
    uint64_t start = ClockCycles(), prev = start;
    const uint64_t end = start + (uint64_t)seconds * cps;

    while (prev < end) {
        uint64_t now = ClockCycles();
        uint64_t gap = now - prev;
        if (gap > threshold) {
            double us = (double)gap * us_per_cycle;
            for (size_t i = 0; i < N_EDGES; i++)
                if (us > EDGES_US[i]) over[i]++;
            lost += gap;
        }
        if (gap > max_gap) max_gap = gap;
        prev = now;
    }

    double wall = (double)(prev - start) * us_per_cycle / 1e6;
    double max_us = (double)max_gap * us_per_cycle;
    double lost_pct = 100.0 * (double)lost * us_per_cycle / 1e6 / wall;

    sp.sched_priority = 10;
    pthread_setschedparam(pthread_self(), SCHED_RR, &sp);

    printf("[%s] cpu=%d prio=%d %.1fs: hueco max=%.1fus tiempo robado=%.2f%%\n",
           label, cpu, prio, wall, max_us, lost_pct);
    printf("  huecos:");
    for (size_t i = 0; i < N_EDGES; i++)
        printf(" >%.0fus=%llu", EDGES_US[i], (unsigned long long)over[i]);
    printf("\n");

    if (summary_path) {
        int is_new = access(summary_path, 0) != 0;
        FILE *s = fopen(summary_path, "a");
        if (!s) {
            perror(summary_path);
            return EXIT_FAILURE;
        }
        if (is_new) {
            fprintf(s, "label,cpu,prio,seconds,max_gap_us,stolen_pct");
            for (size_t i = 0; i < N_EDGES; i++) fprintf(s, ",over_%.0fus", EDGES_US[i]);
            fprintf(s, "\n");
        }
        fprintf(s, "%s,%d,%d,%.1f,%.1f,%.3f", label, cpu, prio, wall, max_us, lost_pct);
        for (size_t i = 0; i < N_EDGES; i++) fprintf(s, ",%llu", (unsigned long long)over[i]);
        fprintf(s, "\n");
        fclose(s);
    }
    return EXIT_SUCCESS;
}
