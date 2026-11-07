/* hmi.c - M6: interfaz y telemetría del ventilador, proceso QNX de prioridad BAJA (10).
 *
 * Cada 10 ms lee los sensores de la planta y, cada 100 ms, los signos vitales (planta), el estado de
 * seguridad (safety_mon) y el de energía (power_mgr). Manda una línea de texto por UDP al host, donde el
 * dashboard la dibuja. Es lo único que el operador "ve".
 *
 * Es deliberadamente prescindible: corre por debajo de todo lo crítico y nadie depende de ella. Si muere,
 * safety_mon la relanza; si se cuelga, solo pierde su propia CPU. Para la demo:
 *   SIGUSR1  -> bug que la hace caer (acceso a un puntero nulo)
 *   SIGUSR2  -> bug que la deja en un bucle infinito, sin bloquearse nunca
 *
 * Uso: hmi -h ip_host [-u puerto] [-s seg] [-p prio]
 */
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

static int try_open(const char *name)
{
    return name_open(name, 0);
}

int main(int argc, char **argv)
{
    const char *host = NULL;
    int port = 5005, prio = 10, opt;
    double secs = 3600;
    while ((opt = getopt(argc, argv, "h:u:s:p:")) != -1) {
        switch (opt) {
        case 'h': host = optarg; break;
        case 'u': port = atoi(optarg); break;
        case 's': secs = atof(optarg); break;
        case 'p': prio = atoi(optarg); break;
        default:
            fprintf(stderr, "uso: %s -h ip_host [-u puerto] [-s seg] [-p prio]\n", PROG);
            return 1;
        }
    }
    if (!host) { fprintf(stderr, "%s: falta -h ip_host\n", PROG); return 1; }
    signal(SIGUSR1, on_sig);
    signal(SIGUSR2, on_sig);
    signal(SIGTERM, on_sig);

    int plant = -1;
    for (int i = 0; i < 300 && plant == -1; i++) {
        plant = try_open(BG_PNEUMATIC_NAME);
        if (plant == -1) usleep(10000);
    }
    if (plant == -1) rt_die(PROG, "name_open(" BG_PNEUMATIC_NAME ")");
    int safety = try_open(BG_SAFETY_NAME), power = try_open(BG_POWER_NAME);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == -1) rt_die(PROG, "socket");
    struct sockaddr_in dst = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (inet_pton(AF_INET, host, &dst.sin_addr) != 1) { fprintf(stderr, "%s: ip inválida\n", PROG); return 1; }

    rt_setup(PROG, prio);
    int chid = ChannelCreate(_NTO_CHF_PRIVATE);
    if (chid == -1) rt_die(PROG, "ChannelCreate");
    timer_t tm = rt_periodic_pulse(PROG, chid, prio, PULSE_TICK, 10000000);

    bg_hdr_t rd = { .type = BG_MSG_READ }, rv = { .type = BG_MSG_VITALS }, rs = { .type = BG_MSG_SAFETY_STATUS },
             rp = { .type = BG_MSG_POWER_STATUS };
    bg_vitals_t vt = { 0 };
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

        sensors_t s;
        if (MsgSend(plant, &rd, sizeof rd, &s, sizeof s) == -1) break;
        if (n % 10 == 0) {
            MsgSend(plant, &rv, sizeof rv, &vt, sizeof vt);
            if (safety == -1) safety = try_open(BG_SAFETY_NAME);
            if (safety != -1 && MsgSend(safety, &rs, sizeof rs, &ss, sizeof ss) == -1) {
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
        int len = snprintf(line, sizeof line,
                           "%.4f,%.3f,%.4f,%.4f,%.2f,%.1f,%.2f,%d,%u,%d,%u,%u,%s,%u,%.4f,%.1f,%d,%.2f,%.4f\n",
                           rt_now_s(), s.paw, s.flow, vt.have ? vt.spo2 : -1.0, vt.etco2, vt.hr, vt.age_s, vt.src,
                           ss.alarms, ss.backup, ss.failovers, ss.hmi_restarts, ss.last_event[0] ? ss.last_event : "-",
                           ps.bits, ps.soc, ps.autonomy_s / 60, (int)getpid(), vt.r, vt.c * 1e3);
        if (sendto(sock, line, (size_t)len, 0, (struct sockaddr *)&dst, sizeof dst) > 0) sent++;
    }
    timer_delete(tm);
    rt_relax();
    printf("%s: %lu muestras, %lu enviadas a %s:%d\n", PROG, n, sent, host, port);
    return 0;
}
