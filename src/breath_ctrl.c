/* breath_ctrl.c - M3/M4: lazo de control de la respiración (PCV o VCV) como proceso QNX.
 *
 * Cada 1 ms (pulso de un timer del kernel):
 *   1. MsgSend(READ)  a pneumatic_sim -> sensores
 *   2. vent_step()    -> actuadores, con el dt medido
 *   3. MsgSend(WRITE) a pneumatic_sim
 *   4. Cada HEARTBEAT_EVERY ciclos, con -S: MsgSendPulse a safety_mon con un contador (latido). Un
 *      pulso no bloquea: el control nunca espera a safety_mon.
 * Se registra el periodo real y el tiempo de cómputo de cada ciclo; el CSV se escribe al final.
 *
 * Uso: breath_ctrl [-s seg] [-p prio] [-m pcv|vcv] [-P p_insp] [-V vt_ml] [-e peep] [-r rr]
 *                  [-T inicio_ciclo] [-S] [-o salida.csv]
 *   -T  instante (s desde el arranque, reloj ClockCycles) en que empezó el ciclo respiratorio; safety_mon
 *       lo pasa al relanzarlo para que siga en fase con la ventilación de respaldo.
 */
#include <getopt.h>
#include <signal.h>
#include <sys/dispatch.h>
#include <unistd.h>

#include "bg_proto.h"
#include "plant.h"
#include "rt_util.h"
#include "vent.h"

#define PROG "breath_ctrl"
#define PULSE_TICK (_PULSE_CODE_MINAVAIL + 0)
#define HEARTBEAT_EVERY 10 /* ciclos entre latidos (~10 ms) */

static volatile sig_atomic_t stop; /* SIGTERM: terminar y escribir resultados */
static void on_term(int sig) { (void)sig; stop = 1; }

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
    double secs = 30, vt_ml = 500, t0 = -1;
    int prio = 50, opt, vcv = 0, heartbeat = 0;
    const char *out_path = NULL;
    vent_settings_t set = { .rr = 12, .ie = 2.0, .p_insp = 20, .peep = 5, .rise_s = 0.1 };
    while ((opt = getopt(argc, argv, "s:p:m:P:V:e:r:T:So:")) != -1) {
        switch (opt) {
        case 's': secs = atof(optarg); break;
        case 'p': prio = atoi(optarg); break;
        case 'm': vcv = optarg[0] == 'v'; break;
        case 'P': set.p_insp = atof(optarg); break;
        case 'V': vt_ml = atof(optarg); break;
        case 'e': set.peep = atof(optarg); break;
        case 'r': set.rr = atof(optarg); break;
        case 'T': t0 = atof(optarg); break;
        case 'S': heartbeat = 1; break;
        case 'o': out_path = optarg; break;
        default:
            fprintf(stderr, "uso: %s [-s seg] [-p prio] [-m pcv|vcv] [-P p_insp] [-V vt_ml] [-e peep] [-r rr] "
                            "[-T inicio_ciclo] [-S] [-o salida.csv]\n", PROG);
            return 1;
        }
    }
    signal(SIGTERM, on_term);

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

    /* Conexión a safety_mon para los latidos. Además, si este proceso muere, el kernel cierra esta
     * conexión y safety_mon recibe _PULSE_CODE_DISCONNECT: así se entera al instante. */
    int safety = -1;
    if (heartbeat) {
        safety = name_open(BG_SAFETY_NAME, 0);
        if (safety == -1) rt_die(PROG, "name_open(" BG_SAFETY_NAME ")");
    }

    rt_setup(PROG, prio);

    int chid = ChannelCreate(_NTO_CHF_PRIVATE);
    if (chid == -1) rt_die(PROG, "ChannelCreate");
    timer_t tm = rt_periodic_pulse(PROG, chid, prio, PULSE_TICK, 1000000);

    vent_t vent;
    double t_start = rt_now_s(), last = t_start;
    if (t0 < 0) t0 = t_start;
    double pbl_max = plant_default_params().pbl_max;
    if (vcv) vent_init_vcv(&vent, &set, vt_ml / 1e3, 40.0, pbl_max, t0);
    else vent_init_pcv(&vent, &set, pbl_max, t0);
    if (vcv)
        printf("%s: pid %d, VCV %.0f mL, PEEP %.0f, %.0f rpm, I:E 1:%.0f, prio %d, %.0f s\n", PROG, getpid(), vt_ml,
               set.peep, set.rr, set.ie, prio, secs);
    else
        printf("%s: pid %d, PCV %.0f/%.0f cmH2O, %.0f rpm, I:E 1:%.0f, prio %d, %.0f s\n", PROG, getpid(),
               set.p_insp, set.peep, set.rr, set.ie, prio, secs);
    fflush(stdout);
    uint32_t beat = 0;

    bg_hdr_t rd = { .type = BG_MSG_READ };
    bg_write_msg_t wr = { .hdr = { .type = BG_MSG_WRITE } };
    struct _pulse pulse;

    for (size_t i = 0; i < cap; i++) {
        if (MsgReceivePulse(chid, &pulse, sizeof pulse, NULL) == -1) {
            if (errno == EINTR) { /* SIGTERM interrumpió la espera: salir limpio */
                if (stop) break;
                continue;
            }
            rt_die(PROG, "MsgReceivePulse");
        }
        double now = rt_now_s();
        double dt = now - last;
        last = now;

        sensors_t s;
        /* MsgSend bloquea hasta que pneumatic_sim responde con MsgReply. */
        if (MsgSend(coid, &rd, sizeof rd, &s, sizeof s) == -1) rt_die(PROG, "MsgSend(READ)");
        vent_step(&vent, now, dt, &s, &wr.act);
        if (MsgSend(coid, &wr, sizeof wr, NULL, 0) == -1) rt_die(PROG, "MsgSend(WRITE)");
        if (safety != -1 && i % HEARTBEAT_EVERY == 0)
            MsgSendPulse(safety, prio, BG_PULSE_HEARTBEAT, (int)beat++);

        logrec_t *r = &log[i];
        r->t = now;
        r->period_ms = (float)(dt * 1e3);
        r->exec_us = (float)((rt_now_s() - now) * 1e6);
        r->paw = (float)s.paw;
        r->flow = (float)s.flow;
        r->target = (float)vent.target;
        r->blower = (float)wr.act.blower;
        r->exh = (float)wr.act.exh_open;
        r->phase = (uint8_t)vent.phase;
        n = i + 1;
        if (now - t_start >= secs || stop) break;
    }

    timer_delete(tm);
    name_close(coid);
    if (safety != -1) {
        MsgSendPulse(safety, prio, BG_PULSE_BYE, 0);
        name_close(safety);
    }
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
