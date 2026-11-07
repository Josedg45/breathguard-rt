/* pneumatic_sim.c - M3: planta neumática + pulmón (RC) como proceso QNX independiente.
 *
 * Hace de "hardware" del ventilador: la física avanza con el reloj real, haya o no controlador.
 * Si breath_ctrl muere, la planta sigue con el último comando, como pasaría con válvulas reales.
 *
 *  - name_attach() registra el nombre BG_PNEUMATIC_NAME y crea el canal.
 *  - Un timer manda un pulso cada 1 ms: la física avanza y se registra una muestra.
 *  - Los mensajes READ, WRITE y FAULT llegan por el mismo canal. Antes de atender cualquiera, la
 *    física se pone al día hasta "ahora", así que lecturas y comandos ocurren en su instante real.
 *  - Acople con Pulse (proceso patient): el tiempo de la planta se corta en ventanas de 20 ms.
 *    patient pide la siguiente ventana con BG_MSG_PATIENT; si aún no se cerró, NO se le responde
 *    hasta cerrarla (respuesta diferida: patient queda bloqueado sin gastar CPU). Con cada
 *    resultado de Pulse se corrige el volumen y se actualizan R, C y V0 del pulmón.
 *  - Arbitraje: safety_mon puede tomar el mando (BG_MSG_OVERRIDE). Mientras lo tiene, los WRITE de
 *    breath_ctrl se guardan pero no se aplican, como un relé de seguridad entre controlador y válvulas.
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
    bg_patient_msg_t pat;
    bg_override_msg_t ov;
} sim_msg_t;

typedef struct {
    double t;
    float dt_ms, paw, q_y, v, pbl, blower, exh, cmd_age_ms, r_pat, c_pat, spo2;
    uint8_t src;  /* quién manda: 0 breath_ctrl, 1 safety_mon */
} logrec_t;

static plant_t pl;
static actuators_t act;       /* lo que se aplica */
static actuators_t act_ctrl;  /* último comando de breath_ctrl */
static int override_on;       /* 1 = manda safety_mon */
static double last_sync;
static unsigned long gaps_clamped;

/* Ventanas de 20 ms para Pulse. Un anillo guarda las últimas para cuando patient va atrasado. */
#define RING 256
typedef struct {
    uint32_t k;
    double t_end, paw_mean, v_end;
} window_t;
static window_t ring[RING];
static uint32_t win_k;          /* ventana en curso */
static double win_end, win_int0; /* fin de la ventana en curso; integral de Paw al empezarla */
static int pending_rcvid = -1;  /* patient esperando la ventana pending_k */
static uint32_t pending_k;
static double last_dv;
static uint32_t lost, stale;
static bg_patient_state_t vitals;
static int have_patient;

static void reply_window(int rcvid, uint32_t k)
{
    if (win_k - k > RING) { /* patient se atrasó más que el anillo: salta a la más vieja */
        lost += win_k - RING - k;
        k = win_k - RING;
    }
    window_t *w = &ring[k % RING];
    bg_window_t rep = { .k = k, .backlog = win_k - k - 1, .t_end = w->t_end,
                        .paw_mean = w->paw_mean, .last_dv = last_dv, .lost = lost };
    MsgReply(rcvid, EOK, &rep, sizeof rep);
}

static void close_window(void)
{
    window_t *w = &ring[win_k % RING];
    w->k = win_k;
    w->t_end = pl.t;
    w->paw_mean = (pl.paw_int - win_int0) / BG_WINDOW_S;
    w->v_end = pl.v;
    win_int0 = pl.paw_int;
    win_end += BG_WINDOW_S;
    win_k++;
    if (pending_rcvid != -1 && pending_k < win_k) {
        reply_window(pending_rcvid, pending_k);
        pending_rcvid = -1;
    }
}

/* Integra la física desde la última vez hasta ahora, cerrando las ventanas que se completen. */
static double sync_to_now(void)
{
    double now = rt_now_s();
    double dt = now - last_sync;
    if (dt > MAX_GAP_S) { dt = MAX_GAP_S; gaps_clamped++; }
    last_sync = now;
    while (dt > 0) {
        double to_end = win_end - pl.t;
        if (dt < to_end) {
            plant_advance(&pl, &act, dt);
            break;
        }
        if (to_end > 0) plant_advance(&pl, &act, to_end);
        dt -= to_end > 0 ? to_end : 0;
        close_window();
    }
    return now;
}

/* Resincroniza el pulmón con el estado de Pulse al final de la ventana k (APRENDIZAJE, M2 paso 6).
 * El resultado llega tarde: la planta ya avanzó. Se corrige el volumen ACTUAL con el error que había
 * al final de la ventana k, y desde ahí sigue con el R y C nuevos. */
static void apply_patient(const bg_patient_msg_t *m)
{
    window_t *w = &ring[m->k % RING];
    if (w->k != m->k) { stale++; return; }
    const bg_patient_state_t *st = &m->st;
    if (st->c <= 0 || st->r <= 0) { stale++; return; }
    last_dv = st->v - w->v_end;
    pl.v += last_dv;
    /* Las ventanas cerradas después de k (si patient va atrasado) guardaron el volumen sin esta
     * corrección; se desplazan también, o la siguiente corrección contaría el mismo error otra vez. */
    for (uint32_t j = m->k + 1; j < win_k; j++) ring[j % RING].v_end += last_dv;
    double palv = w->paw_mean - st->q * st->r; /* presión alveolar de Pulse en ese instante */
    pl.p.v0 = st->v - st->c * palv;
    pl.p.c_pat = st->c;
    pl.p.r_pat = st->r;
    vitals = *st;
    have_patient = 1;
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
    win_end = BG_WINDOW_S;
    unsigned long reads = 0, writes = 0, overrides = 0;
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
                    r->r_pat = (float)pl.p.r_pat;
                    r->c_pat = (float)(pl.p.c_pat * 1e3);
                    r->spo2 = have_patient ? (float)vitals.spo2 : -1.0f;
                    r->src = (uint8_t)override_on;
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
            act_ctrl = m.write.act;
            if (!override_on) act = act_ctrl;
            writes++;
            MsgReply(rcvid, EOK, NULL, 0);
            break;
        case BG_MSG_PATIENT: {
            sync_to_now();
            if (m.pat.have_result) apply_patient(&m.pat);
            uint32_t want = m.pat.have_result ? m.pat.k + 1 : win_k;
            if (want < win_k) {
                reply_window(rcvid, want);
            } else { /* aún no se cierra: se responde en close_window() */
                if (pending_rcvid != -1) MsgError(pending_rcvid, EBUSY);
                pending_rcvid = rcvid;
                pending_k = want;
            }
            break;
        }
        case BG_MSG_OVERRIDE:
            sync_to_now();
            override_on = m.ov.on;
            act = override_on ? m.ov.act : act_ctrl;
            overrides++;
            MsgReply(rcvid, EOK, NULL, 0);
            break;
        case BG_MSG_FAULT:
            sync_to_now();
            printf("%s: falla t_abs=%.6f desconectado=%d obstruccion=%.1f fuga=%.3f sensor=%d\n", PROG,
                   rt_now_s(), m.fault.faults.disconnected, m.fault.faults.obstruction, m.fault.faults.g_leak,
                   m.fault.faults.paw_stuck);
            fflush(stdout);
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
    printf("%s: ventanas %u, paciente %s, ventanas perdidas %u, resultados descartados %u\n", PROG, win_k,
           have_patient ? "conectado" : "sin Pulse (RC fijo)", lost, stale);
    printf("%s: mensajes de override %lu\n", PROG, overrides);

    if (out_path) {
        FILE *f = fopen(out_path, "w");
        if (!f) rt_die(PROG, out_path);
        fprintf(f, "t_s,dt_ms,paw,q_y,v_l,pbl,blower,exh_open,cmd_age_ms,r_pat,c_pat_ml,spo2,src\n");
        for (size_t i = 0; i < n; i++) {
            logrec_t *r = &log[i];
            fprintf(f, "%.6f,%.3f,%.3f,%.4f,%.5f,%.3f,%.4f,%.2f,%.2f,%.2f,%.2f,%.4f,%u\n", r->t, r->dt_ms,
                    r->paw, r->q_y, r->v, r->pbl, r->blower, r->exh, r->cmd_age_ms, r->r_pat,
                    r->c_pat, r->spo2, r->src);
        }
        fclose(f);
    }
    return 0;
}
