/* step_test.c - escalones en lazo abierto sobre pneumatic_sim en QNX, para identificar el sistema (design/).
 * Mismo calendario que tools/design_ref.c (step_schedule): cada 1 ms (pulso de un timer) lee los sensores,
 * escribe el comando del calendario y registra ambos. La planta registra su estado verdadero en su propio CSV.
 *
 * Uso: step_test [-p prio] [-o salida.csv]   (16 s; lanzar pneumatic_sim antes)
 */
#include <getopt.h>
#include <sys/dispatch.h>
#include <unistd.h>

#include "bg_proto.h"
#include "plant.h"
#include "rt_util.h"

#define PROG "step_test"
#define PULSE_TICK (_PULSE_CODE_MINAVAIL + 0)
#define PEEP0 5.0
#define SECS 16.0

static void step_schedule(double t, double pbl_max, actuators_t *a)
{
    a->peep_cmd = PEEP0;
    a->blower = (t >= 1.0 && t < 10.0 ? 20.0 : PEEP0) / pbl_max;
    a->exh_open = t >= 10.0 ? 1.0 : 0.0;
}

typedef struct {
    double t;
    float paw, flow, blower, exh;
} rec_t;

int main(int argc, char **argv)
{
    int prio = 50, opt;
    const char *out_path = NULL;
    while ((opt = getopt(argc, argv, "p:o:")) != -1) {
        switch (opt) {
        case 'p': prio = atoi(optarg); break;
        case 'o': out_path = optarg; break;
        default: fprintf(stderr, "uso: %s [-p prio] [-o salida.csv]\n", PROG); return 1;
        }
    }
    size_t cap = (size_t)(SECS * 1000) + 1000, n = 0;
    rec_t *log = calloc(cap, sizeof *log);
    if (!log) rt_die(PROG, "calloc");
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

    const double pbl_max = plant_default_params().pbl_max;
    bg_hdr_t rd = { .type = BG_MSG_READ };
    bg_write_msg_t wr = { .hdr = { .type = BG_MSG_WRITE } };
    struct _pulse pulse;
    double t_start = rt_now_s();
    while (n < cap) {
        if (MsgReceivePulse(chid, &pulse, sizeof pulse, NULL) == -1) rt_die(PROG, "MsgReceivePulse");
        double now = rt_now_s(), t = now - t_start;
        sensors_t s;
        if (MsgSend(coid, &rd, sizeof rd, &s, sizeof s) == -1) rt_die(PROG, "MsgSend(READ)");
        step_schedule(t, pbl_max, &wr.act);
        if (MsgSend(coid, &wr, sizeof wr, NULL, 0) == -1) rt_die(PROG, "MsgSend(WRITE)");
        log[n++] = (rec_t){ now, (float)s.paw, (float)s.flow, (float)wr.act.blower, (float)wr.act.exh_open };
        if (t >= SECS) break;
    }
    timer_delete(tm);
    rt_relax();
    printf("%s: %zu ciclos\n", PROG, n);
    if (out_path) {
        FILE *f = fopen(out_path, "w");
        if (!f) rt_die(PROG, out_path);
        fprintf(f, "t_s,paw_meas,flow_meas,blower,exh_open\n");
        for (size_t i = 0; i < n; i++)
            fprintf(f, "%.6f,%.3f,%.4f,%.4f,%.2f\n", log[i].t, log[i].paw, log[i].flow, log[i].blower, log[i].exh);
        fclose(f);
    }
    return 0;
}
