/*
 * cpu_burn.c - carga basura: un hilo SCHED_FIFO girando por cada CPU.
 *
 * Al terminar reporta el % de CPU que recibió cada hilo: es la evidencia de
 * que la máquina estuvo realmente saturada durante la medición.
 */
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syspage.h>
#include <time.h>

#define BUF_SIZE (4u * 1024u * 1024u) /* también ensucia la caché, no solo la ALU */

struct worker {
    pthread_t tid;
    double seconds;
    double cpu_pct;
};

static double now_s(clockid_t clk)
{
    struct timespec ts;
    clock_gettime(clk, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void *burn(void *arg)
{
    struct worker *w = arg;
    volatile uint8_t *buf = malloc(BUF_SIZE);
    if (!buf) {
        perror("cpu_burn: malloc");
        return NULL;
    }

    double start = now_s(CLOCK_MONOTONIC);
    double cpu_start = now_s(CLOCK_THREAD_CPUTIME_ID);
    double end = start + w->seconds;
    size_t idx = 0;

    while (now_s(CLOCK_MONOTONIC) < end) {
        for (int i = 0; i < 4096; i++) {
            buf[idx] = (uint8_t)(buf[idx] + 1);
            idx = (idx + 64) % BUF_SIZE;
        }
    }

    double wall = now_s(CLOCK_MONOTONIC) - start;
    w->cpu_pct = 100.0 * (now_s(CLOCK_THREAD_CPUTIME_ID) - cpu_start) / wall;
    free((void *)buf);
    return NULL;
}

int main(int argc, char **argv)
{
    int threads = _syspage_ptr->num_cpu, prio = 9, opt;
    double seconds = 12.0;

    while ((opt = getopt(argc, argv, "n:s:p:")) != -1) {
        switch (opt) {
        case 'n': threads = atoi(optarg); break;
        case 's': seconds = atof(optarg); break;
        case 'p': prio = atoi(optarg); break;
        default:
            fprintf(stderr, "uso: cpu_burn [-n hilos] [-s segundos] [-p prioridad]\n");
            return EXIT_FAILURE;
        }
    }
    if (threads <= 0 || seconds <= 0 || prio <= 0) {
        fprintf(stderr, "cpu_burn: argumentos inválidos\n");
        return EXIT_FAILURE;
    }

    struct worker *w = calloc((size_t)threads, sizeof *w);
    if (!w) {
        perror("cpu_burn: calloc");
        return EXIT_FAILURE;
    }

    pthread_attr_t attr;
    struct sched_param sp = { .sched_priority = prio };
    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    pthread_attr_setschedparam(&attr, &sp);

    for (int i = 0; i < threads; i++) {
        w[i].seconds = seconds;
        int rc = pthread_create(&w[i].tid, &attr, burn, &w[i]);
        if (rc != 0) {
            fprintf(stderr, "cpu_burn: pthread_create: %s\n", strerror(rc));
            return EXIT_FAILURE;
        }
    }

    for (int i = 0; i < threads; i++) pthread_join(w[i].tid, NULL);
    for (int i = 0; i < threads; i++)
        printf("cpu_burn: hilo %d prio=%d SCHED_FIFO cpu=%.1f%%\n", i, prio, w[i].cpu_pct);

    free(w);
    return EXIT_SUCCESS;
}
