/* hmi.c - M6: interfaz y telemetría del ventilador, proceso QNX de prioridad BAJA (10).
 *
 * Cada 10 ms pide a safety_mon la última lectura de los sensores y los signos vitales (BG_MSG_MONITOR) y, cada
 * 100 ms, el estado de seguridad (safety_mon) y el de energía (power_mgr). No se conecta con la planta. Manda una línea de texto por UDP al host, donde el
 * dashboard la dibuja. Es lo único que el operador "ve".
 *
 * Es deliberadamente prescindible: corre por debajo de todo lo crítico y nadie depende de ella. Si muere,
 * safety_mon la relanza; si se cuelga, solo pierde su propia CPU. Para la demo:
 *   SIGUSR1  -> bug que la hace caer (acceso a un puntero nulo)
 *   SIGUSR2  -> bug que la deja en un bucle infinito, sin bloquearse nunca
 *
 * Comandos del operador: escucha en UDP (puerto -c, 5006) lineas como "p_insp=25 peep=8" (claves: mode=pcv|vcv,
 * p_insp, peep, rr, ie, vt en mL). NO valida ni aplica nada: arma el pedido sobre los ajustes actuales y lo
 * reenvia a safety_mon, que decide. Al remitente le contesta "ACK ..." o "NAK motivo".
 *
 * Una vez por segundo manda ademas una linea "RT,clave=valor,..." con las metricas de tiempo real del lazo
 * (periodo p50/p99/max, plazos incumplidos, histograma), la antigüedad del comando en la planta, el peso ideal,
 * la FiO2, el auto-PEEP estimado y los limites de alarma activos.
 *
 * Uso: hmi -h ip_host [-u puerto] [-c puerto_comandos] [-s seg] [-p prio]
 */
#include <fcntl.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/dispatch.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bg_power.h"
#include "bg_proto.h"
#include "rt_util.h"

#define PROG "hmi"
#define PULSE_TICK (_PULSE_CODE_MINAVAIL + 0)

static volatile sig_atomic_t bug, stop;
static void on_sig(int sig)
{
    if (sig == SIGUSR1) bug = 1;
    if (sig == SIGUSR2) bug = 2;
    if (sig == SIGTERM) stop = 1;
}

/* Un comando de texto -> pedido de ajustes, partiendo de los actuales. Devuelve 0 si se entendio. */
static int parse_cmd(char *txt, const bg_settings_t *base, bg_settings_t *req, char *err, size_t n)
{
    *req = *base;
    int any = 0;
    for (char *tok = strtok(txt, " \t\r\n,;"); tok; tok = strtok(NULL, " \t\r\n,;")) {
        char *eq = strchr(tok, '=');
        if (!eq) { snprintf(err, n, "falta '=' en '%.20s'", tok); return -1; }
        *eq = 0;
        const char *k = tok, *v = eq + 1;
        char *end;
        double x = strtod(v, &end);
        if (!strcmp(k, "mode")) {
            if (!strcmp(v, "pcv")) req->vcv = 0;
            else if (!strcmp(v, "vcv")) req->vcv = 1;
            else { snprintf(err, n, "modo '%.8s' desconocido", v); return -1; }
        } else if (end == v || *end) { snprintf(err, n, "valor '%.12s' no numerico", v); return -1; }
        else if (!strcmp(k, "p_insp")) req->set.p_insp = x;
        else if (!strcmp(k, "peep")) req->set.peep = x;
        else if (!strcmp(k, "rr")) req->set.rr = x;
        else if (!strcmp(k, "ie")) req->set.ie = x;
        else if (!strcmp(k, "vt")) req->vt = x / 1e3;
        else { snprintf(err, n, "clave '%.12s' desconocida", k); return -1; }
        any = 1;
    }
    if (!any) { snprintf(err, n, "comando vacio"); return -1; }
    return 0;
}

static int try_open(const char *name)
{
    return name_open(name, 0);
}

int main(int argc, char **argv)
{
    const char *host = NULL;
    int port = 5005, cmd_port = 5006, prio = 10, opt;
    double secs = 3600;
    while ((opt = getopt(argc, argv, "h:u:c:s:p:")) != -1) {
        switch (opt) {
        case 'h': host = optarg; break;
        case 'u': port = atoi(optarg); break;
        case 'c': cmd_port = atoi(optarg); break;
        case 's': secs = atof(optarg); break;
        case 'p': prio = atoi(optarg); break;
        default:
            fprintf(stderr, "uso: %s -h ip_host [-u puerto] [-c puerto_comandos] [-s seg] [-p prio]\n", PROG);
            return 1;
        }
    }
    if (!host) { fprintf(stderr, "%s: falta -h ip_host\n", PROG); return 1; }
    signal(SIGUSR1, on_sig);
    signal(SIGUSR2, on_sig);
    signal(SIGTERM, on_sig);

    /* La HMI no abre conexión con la planta (tarea 4): ve sensores y signos vitales a través de safety_mon. Si la
     * HMI se compromete, no tiene un canal hacia los actuadores. */
    int safety = -1;
    for (int i = 0; i < 300 && safety == -1; i++) {
        safety = try_open(BG_SAFETY_NAME);
        if (safety == -1) usleep(10000);
    }
    if (safety == -1) rt_die(PROG, "name_open(" BG_SAFETY_NAME ")");
    int power = try_open(BG_POWER_NAME);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == -1) rt_die(PROG, "socket");
    struct sockaddr_in dst = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (inet_pton(AF_INET, host, &dst.sin_addr) != 1) { fprintf(stderr, "%s: ip inválida\n", PROG); return 1; }

    /* Socket de comandos: no bloqueante, se revisa en cada tick. */
    int csock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons((uint16_t)cmd_port),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    int one = 1;
    if (csock != -1) setsockopt(csock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (csock == -1 || bind(csock, (struct sockaddr *)&me, sizeof me) == -1) rt_die(PROG, "bind(comandos)");
    fcntl(csock, F_SETFL, fcntl(csock, F_GETFL) | O_NONBLOCK);
    unsigned long cmds = 0;

    rt_setup(PROG, prio);
    int chid = ChannelCreate(_NTO_CHF_PRIVATE);
    if (chid == -1) rt_die(PROG, "ChannelCreate");
    timer_t tm = rt_periodic_pulse(PROG, chid, prio, PULSE_TICK, 10000000);

    bg_hdr_t rmo = { .type = BG_MSG_MONITOR }, rs = { .type = BG_MSG_SAFETY_STATUS },
             rp = { .type = BG_MSG_POWER_STATUS };
    bg_monitor_t mo = { 0 };
    bg_safety_status_t ss = { 0 };
    bg_power_status_t ps = { 0 };
    struct _pulse pulse;
    double t_start = rt_now_s();
    unsigned long n = 0, sent = 0;
    char line[512];

    while (!stop && rt_now_s() - t_start < secs) {
        if (MsgReceivePulse(chid, &pulse, sizeof pulse, NULL) == -1) {
            if (errno == EINTR) continue; /* una señal interrumpe la espera: no es un error */
            rt_die(PROG, "MsgReceivePulse");
        }
        if (bug == 1) { /* bug de la demo: acceso a un puntero nulo -> SIGSEGV */
            volatile int *p = NULL;
            *p = 42;
        }
        if (bug == 2) { /* bug de la demo: bucle infinito que nunca se bloquea */
            volatile unsigned long spin = 0;
            for (;;) spin++;
        }

        if (safety == -1) safety = try_open(BG_SAFETY_NAME);
        if (safety == -1 || MsgSend(safety, &rmo, sizeof rmo, &mo, sizeof mo) == -1) {
            if (safety != -1) name_close(safety);
            safety = -1;
            continue; /* sin safety_mon no hay datos que mostrar */
        }
        const sensors_t s = mo.s;
        if (n % 10 == 0) {
            if (MsgSend(safety, &rs, sizeof rs, &ss, sizeof ss) == -1) {
                name_close(safety);
                safety = -1;
            }
            if (power == -1 && n % 100 == 0) power = try_open(BG_POWER_NAME);
            if (power != -1 && MsgSend(power, &rp, sizeof rp, &ps, sizeof ps) == -1) {
                name_close(power);
                power = -1;
            }
        }
        n++;

        /* Comandos del operador pendientes en el socket. */
        struct sockaddr_in from;
        socklen_t flen = sizeof from;
        char cbuf[256], ans[160];
        ssize_t cl;
        while ((cl = recvfrom(csock, cbuf, sizeof cbuf - 1, 0, (struct sockaddr *)&from, &flen)) > 0) {
            cbuf[cl] = 0;
            cmds++;
            bg_settings_msg_t sm = { .hdr = { .type = BG_MSG_SETTINGS_SET } };
            bg_settings_reply_t rep;
            char err[64];
            if (safety == -1) snprintf(ans, sizeof ans, "NAK safety_mon no disponible\n");
            else if (parse_cmd(cbuf, &ss.set, &sm.req, err, sizeof err) == -1) snprintf(ans, sizeof ans, "NAK %s\n", err);
            else if (MsgSend(safety, &sm, sizeof sm, &rep, sizeof rep) == -1) snprintf(ans, sizeof ans, "NAK MsgSend\n");
            else if (!rep.ok) snprintf(ans, sizeof ans, "NAK %s\n", rep.why);
            else {
                ss.set = rep.now; /* para que un segundo comando seguido parta de este */
                snprintf(ans, sizeof ans, "ACK #%u %s p_insp %.1f peep %.1f rr %.1f ie %.1f vt %.0f desde t=%.3f\n",
                         rep.now.seq, rep.now.vcv ? "VCV" : "PCV", rep.now.set.p_insp, rep.now.set.peep,
                         rep.now.set.rr, rep.now.set.ie, rep.now.vt * 1e3, rep.now.t0);
            }
            sendto(csock, ans, strlen(ans), 0, (struct sockaddr *)&from, flen);
            flen = sizeof from;
        }

        const bg_settings_t *st = &ss.set;
        int len = snprintf(line, sizeof line,
                           "%.4f,%.3f,%.4f,%.4f,%.2f,%.1f,%.2f,%d,%u,%d,%u,%u,%s,%u,%.4f,%.1f,%d,%.2f,%.4f,"
                           "%d,%.1f,%.1f,%.1f,%.1f,%.0f,%u,%d,%.2f\n",
                           rt_now_s(), s.paw, s.flow, mo.vt.have ? mo.vt.spo2 : -1.0, mo.vt.etco2, mo.vt.hr,
                           mo.vt.age_s, mo.vt.src,
                           ss.alarms, ss.backup, ss.failovers, ss.hmi_restarts, ss.last_event[0] ? ss.last_event : "-",
                           ps.bits, ps.soc, ps.autonomy_s / 60, (int)getpid(), mo.vt.r, mo.vt.c * 1e3,
                           st->vcv, st->set.p_insp, st->set.peep, st->set.rr, st->set.ie, st->vt * 1e3, st->seq,
                           ss.pending, mo.vt.cmd_age_max * 1e3);
        if (sendto(sock, line, (size_t)len, 0, (struct sockaddr *)&dst, sizeof dst) > 0) sent++;

        if (n % 100 == 1) { /* 1 Hz: metricas y limites (cambian despacio) */
            const bg_loop_stats_t *L = &ss.loop;
            char rt[1024];
            int k = snprintf(rt, sizeof rt,
                             "RT,t=%.3f,pid=%d,cyc=%u,late=%u,lost=%u,p50=%.3f,p99=%.3f,p999=%.3f,max=%.3f,"
                             "ex99=%.1f,exmax=%.1f,loop_age=%.2f,age99=%.2f,agemax=%.2f,fio2=%.2f,ibw=%.1f,"
                             "ap=%.2f,qee=%.4f,tau=%.3f,eef=%d,lim_phi=%.1f,lim_plo=%.1f,lim_apnea=%.1f,"
                             "lim_vtlo=%.0f,lim_vthi=%.0f,lim_ap=%.1f,denied=%u,hist=",
                             rt_now_s(), L->pid, L->cycles, L->late, L->lost, L->p50_ms, L->p99_ms, L->p999_ms,
                             L->max_ms, L->exec_p99_us, L->exec_max_us, ss.loop_age_s, mo.vt.cmd_age_p99_all * 1e3,
                             mo.vt.cmd_age_max_all * 1e3, mo.vt.fio2, ss.ibw_kg, ss.auto_peep, ss.q_ee, ss.tau_exp,
                             ss.eef, ss.lim_p_high, ss.lim_p_low, ss.lim_apnea_s, ss.lim_vt_lo * 1e3,
                             ss.lim_vt_hi * 1e3, ss.lim_auto_peep, ss.denied);
            for (int h = 0; h < BG_HIST_BINS && k < (int)sizeof rt - 16; h++)
                k += snprintf(rt + k, sizeof rt - k, h ? "|%u" : "%u", L->hist[h]);
            k += snprintf(rt + k, sizeof rt - k, "\n");
            sendto(sock, rt, (size_t)k, 0, (struct sockaddr *)&dst, sizeof dst);
        }
    }
    timer_delete(tm);
    rt_relax();
    printf("%s: %lu muestras, %lu enviadas a %s:%d, %lu comandos\n", PROG, n, sent, host, port, cmds);
    return 0;
}
