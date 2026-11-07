/* pneumatic_sim.c - M3: planta neumática + pulmón (RC) como proceso QNX independiente.
 *
 * Hace de "hardware" del ventilador: la física avanza con el reloj real, haya o no controlador.
 * Si breath_ctrl muere, la planta sigue con el último comando, como pasaría con válvulas reales.
 *
 *  - name_attach() registra el nombre BG_PNEUMATIC_NAME y crea el canal.
 *  - Un timer manda un pulso cada 1 ms: la física avanza y se registra una muestra.
 *  - Los mensajes READ, WRITE y FAULT llegan por el mismo canal. Antes de atender cualquiera, la
 *    física se pone al día hasta "ahora", así que lecturas y comandos ocurren en su instante real.
 *
 * Uso: pneumatic_sim [-s segundos] [-p prio] [-e peep_inicial] [-o salida.csv]
 */
#include <getopt.h>
#include <sys/dispatch.h>
#include <unistd.h>

#include "bg_proto.h"
#include "plant.h"
#include "rt_util.h"

#define PROG "pneumatic_sim"
#define PULSE_TICK (_PULSE_CODE_MINAVAIL + 0)
#define MAX_GAP_S 0.1 /* si la planta pasa más de esto sin correr, no integra el exceso */

typedef union {
    bg_hdr_t hdr;
    struct _pulse pulse;
    bg_write_msg_t write;
    bg_fault_msg_t fault;
} sim_msg_t;

typedef struct {
    double t;
    float dt_ms, paw, q_y, v, pbl, blower, exh, cmd_age_ms;
} logrec_t;

static plant_t pl;
static actuators_t act;
static double last_sync;
static unsigned long gaps_clamped;

/* Integra la física desde la última vez hasta ahora. */
static double sync_to_now(void)
{
    double now = rt_now_s();
    double dt = now - last_sync;
    if (dt > MAX_GAP_S) { dt = MAX_GAP_S; gaps_clamped++; }
    if (dt > 0) plant_advance(&pl, &act, dt);
    last_sync = now;
    return now;
}

int main(int argc, char **argv)
{
    double secs = 30, peep0 = 5;
    int prio = 60, opt;
    const char *out_path = NULL;
    while ((opt = getopt(argc, argv, "s:p:e:o:")) != -1) {
        switch (opt) {
        case 's': secs = atof(optarg); break;
        case 'p': prio = atoi(optarg); break;
        case 'e': peep0 = atof(optarg); break;
        case 'o': out_path = optarg; break;
        default:
            fprintf(stderr, "uso: %s [-s segundos] [-p prio] [-e peep_inicial] [-o salida.csv]\n", PROG);
            return 1;
        }
    }

    size_t cap = (size_t)(secs * 1000) + 1000, n = 0;
    logrec_t *log = calloc(cap, sizeof *log);
    if (!log) rt_die(PROG, "calloc");

    plant_params_t pp = plant_default_params();
    plant_init(&pl, &pp, peep0);
    /* Hasta que hable el controlador: soplador a la PEEP y válvula espiratoria abierta. */
    act.blower = peep0 / pp.pbl_max;
    act.exh_open = 1.0;
    act.peep_cmd = peep0;

    rt_setup(PROG, prio);

    /* name_attach: crea un canal y lo publica bajo un nombre (en /dev/name/local/). Así los
     * clientes no necesitan conocer nuestro pid ni el número de canal. */
    name_attach_t *att = name_attach(NULL, BG_PNEUMATIC_NAME, 0);
    if (!att) rt_die(PROG, "name_attach");

    timer_t tm = rt_periodic_pulse(PROG, att->chid, prio, PULSE_TICK, 1000000);

    double t_start = rt_now_s(), last_write = t_start;
    last_sync = t_start;
    unsigned long reads = 0, writes = 0;
    printf("%s: listo como '%s', prio %d, %.0f s\n", PROG, BG_PNEUMATIC_NAME, prio, secs);
    fflush(stdout);

    sim_msg_t m;
    for (;;) {
        int rcvid = MsgReceive(att->chid, &m, sizeof m, NULL);
        if (rcvid == -1) rt_die(PROG, "MsgReceive");

        if (rcvid == 0) { /* pulso */
            if (m.pulse.code == PULSE_TICK) {
                double prev = last_sync;
                double now = sync_to_now();
                if (n < cap) {
                    logrec_t *r = &log[n++];
                    r->t = now;
                    r->dt_ms = (float)((now - prev) * 1e3);
                    r->paw = (float)pl.paw;
                    r->q_y = (float)pl.q_y;
                    r->v = (float)pl.v;
                    r->pbl = (float)pl.pbl;
                    r->blower = (float)act.blower;
                    r->exh = (float)act.exh_open;
                    r->cmd_age_ms = (float)((now - last_write) * 1e3);
                }
                if (now - t_start >= secs) break;
            } else if (m.pulse.code == _PULSE_CODE_DISCONNECT) {
                /* Un cliente cerró su conexión (o murió): liberamos su lado. */
                ConnectDetach(m.pulse.scoid);
            }
            continue;
        }

        if (m.hdr.type == _IO_CONNECT) { /* name_open() de un cliente */
            MsgReply(rcvid, EOK, NULL, 0);
            continue;
        }

        switch (m.hdr.type) {
        case BG_MSG_READ: {
            sync_to_now();
            sensors_t s;
            plant_sense(&pl, &s);
            reads++;
            MsgReply(rcvid, EOK, &s, sizeof s);
            break;
        }
        case BG_MSG_WRITE:
            last_write = sync_to_now(); /* lo anterior se integra con el comando viejo */
            act = m.write.act;
            writes++;
            MsgReply(rcvid, EOK, NULL, 0);
            break;
        case BG_MSG_FAULT:
            sync_to_now();
            pl.f = m.fault.faults;
            MsgReply(rcvid, EOK, NULL, 0);
            break;
        default:
            MsgError(rcvid, ENOSYS);
        }
    }

    timer_delete(tm);
    name_detach(att, 0);
    rt_relax();

    float dt_max = 0;
    unsigned long late = 0;
    for (size_t i = 1; i < n; i++) {
        if (log[i].dt_ms > dt_max) dt_max = log[i].dt_ms;
        if (log[i].dt_ms > 1.5f) late++;
    }
    printf("%s: %zu ticks, %lu > 1.5 ms, dt máx %.2f ms, huecos recortados %lu, lecturas %lu, comandos %lu\n",
           PROG, n, late, dt_max, gaps_clamped, reads, writes);

    if (out_path) {
        FILE *f = fopen(out_path, "w");
        if (!f) rt_die(PROG, out_path);
        fprintf(f, "t_s,dt_ms,paw,q_y,v_l,pbl,blower,exh_open,cmd_age_ms\n");
        for (size_t i = 0; i < n; i++) {
            logrec_t *r = &log[i];
            fprintf(f, "%.6f,%.3f,%.3f,%.4f,%.5f,%.3f,%.4f,%.2f,%.2f\n", r->t, r->dt_ms,
                    r->paw, r->q_y, r->v, r->pbl, r->blower, r->exh, r->cmd_age_ms);
        }
        fclose(f);
    }
    return 0;
}
