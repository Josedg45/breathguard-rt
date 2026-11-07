/* breath_ctrl.c - M3: lazo de control de la respiración (PCV) como proceso QNX.
 *
 * Cada 1 ms (pulso de un timer del kernel):
 *   1. MsgSend(READ)  a pneumatic_sim -> sensores
 *   2. pcv_step()     -> actuadores, con el dt medido
 *   3. MsgSend(WRITE) a pneumatic_sim
 * Se registra el periodo real y el tiempo de cómputo de cada ciclo; el CSV se escribe al final.
 *
 * Uso: breath_ctrl [-s segundos] [-p prio] [-P p_insp] [-e peep] [-r rr] [-o salida.csv]
 */
#include <getopt.h>
#include <sys/dispatch.h>
#include <unistd.h>

#include "bg_proto.h"
#include "pcv.h"
#include "plant.h"
#include "rt_util.h"

#define PROG "breath_ctrl"
#define PULSE_TICK (_PULSE_CODE_MINAVAIL + 0)

typedef struct {
    double t;
    float period_ms, exec_us, paw, flow, target, blower, exh;
    uint8_t phase;
} logrec_t;

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    double secs = 30;
    int prio = 50, opt;
    const char *out_path = NULL;
    vent_settings_t set = { .rr = 12, .ie = 2.0, .p_insp = 20, .peep = 5, .rise_s = 0.1 };
    while ((opt = getopt(argc, argv, "s:p:P:e:r:o:")) != -1) {
        switch (opt) {
        case 's': secs = atof(optarg); break;
        case 'p': prio = atoi(optarg); break;
        case 'P': set.p_insp = atof(optarg); break;
        case 'e': set.peep = atof(optarg); break;
        case 'r': set.rr = atof(optarg); break;
        case 'o': out_path = optarg; break;
        default:
            fprintf(stderr, "uso: %s [-s seg] [-p prio] [-P p_insp] [-e peep] [-r rr] [-o salida.csv]\n", PROG);
            return 1;
        }
    }

    /* Termina por tiempo de reloj, no por número de ciclos: en la VM se pierden pulsos del timer
     * (M1), y contar ciclos alargaba la corrida más allá de lo pedido. */
    size_t cap = (size_t)(secs * 1000) + 1000, n = 0;
    logrec_t *log = calloc(cap, sizeof *log);
    float *tmp = calloc(cap, sizeof *tmp);
    if (!log || !tmp) rt_die(PROG, "calloc");

    /* name_open: busca el nombre publicado por pneumatic_sim y nos conecta a su canal.
     * Reintentamos un rato por si la planta aún está arrancando. */
    int coid = -1;
    for (int i = 0; i < 500 && coid == -1; i++) {
        coid = name_open(BG_PNEUMATIC_NAME, 0);
        if (coid == -1) usleep(10000);
    }
    if (coid == -1) rt_die(PROG, "name_open(" BG_PNEUMATIC_NAME ")");

    rt_setup(PROG, prio);

    int chid = ChannelCreate(_NTO_CHF_PRIVATE);
    if (chid == -1) rt_die(PROG, "ChannelCreate");
    timer_t tm = rt_periodic_pulse(PROG, chid, prio, PULSE_TICK, 1000000);

    pcv_t pcv;
    double t_start = rt_now_s(), last = t_start;
    pcv_init(&pcv, &set, plant_default_params().pbl_max, t_start);
    printf("%s: PCV %.0f/%.0f cmH2O, %.0f rpm, I:E 1:%.0f, prio %d, %.0f s\n", PROG, set.p_insp,
           set.peep, set.rr, set.ie, prio, secs);
    fflush(stdout);

    bg_hdr_t rd = { .type = BG_MSG_READ };
    bg_write_msg_t wr = { .hdr = { .type = BG_MSG_WRITE } };
    struct _pulse pulse;

    for (size_t i = 0; i < cap; i++) {
        if (MsgReceivePulse(chid, &pulse, sizeof pulse, NULL) == -1) rt_die(PROG, "MsgReceivePulse");
        double now = rt_now_s();
        double dt = now - last;
        last = now;

        sensors_t s;
        /* MsgSend bloquea hasta que pneumatic_sim responde con MsgReply. */
        if (MsgSend(coid, &rd, sizeof rd, &s, sizeof s) == -1) rt_die(PROG, "MsgSend(READ)");
        pcv_step(&pcv, now, dt, &s, &wr.act);
        if (MsgSend(coid, &wr, sizeof wr, NULL, 0) == -1) rt_die(PROG, "MsgSend(WRITE)");

        logrec_t *r = &log[i];
        r->t = now;
        r->period_ms = (float)(dt * 1e3);
        r->exec_us = (float)((rt_now_s() - now) * 1e6);
        r->paw = (float)s.paw;
        r->flow = (float)s.flow;
        r->target = (float)pcv.target;
        r->blower = (float)wr.act.blower;
        r->exh = (float)wr.act.exh_open;
        r->phase = (uint8_t)pcv.phase;
        n = i + 1;
        if (now - t_start >= secs) break;
    }

    timer_delete(tm);
    name_close(coid);
    rt_relax();

    /* Resumen: periodo (desde el 2.º ciclo) y tiempo de cómputo. */
    size_t late = 0;
    float pmax = 0;
    for (size_t i = 1; i < n; i++) {
        if (log[i].period_ms > 1.5f) late++;
        if (log[i].period_ms > pmax) pmax = log[i].period_ms;
    }
    for (size_t i = 0; i < n; i++) tmp[i] = log[i].exec_us;
    qsort(tmp, n, sizeof *tmp, cmp_float);
    printf("%s: %zu ciclos, %zu con periodo > 1.5 ms, periodo máx %.2f ms; cómputo p50 %.1f us, "
           "p99 %.1f us, máx %.1f us\n", PROG, n, late, pmax, tmp[n / 2], tmp[(size_t)(n * 0.99)], tmp[n - 1]);

    if (out_path) {
        FILE *f = fopen(out_path, "w");
        if (!f) rt_die(PROG, out_path);
        fprintf(f, "t_s,period_ms,exec_us,phase,target,paw_meas,flow_meas,blower,exh_open\n");
        for (size_t i = 0; i < n; i++) {
            logrec_t *r = &log[i];
            fprintf(f, "%.6f,%.3f,%.1f,%u,%.3f,%.3f,%.4f,%.4f,%.2f\n", r->t, r->period_ms,
                    r->exec_us, r->phase, r->target, r->paw, r->flow, r->blower, r->exh);
        }
        fclose(f);
    }
    return 0;
}
