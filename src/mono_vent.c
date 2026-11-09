/* mono_vent.c - M6: version "monolitica" del ventilador, solo para el contraste de la demo.
 *
 * Hace lo mismo que breath_ctrl + hmi, pero en UN solo proceso y UN solo lazo, como el "superloop" de un
 * microcontrolador (Arduino/ESP32): cada 1 ms controla la respiracion y cada 10 ms hace el trabajo de la
 * pantalla/telemetria. No hay safety_mon ni nadie que lo vigile.
 *
 * Los mismos bugs de la demo que en hmi.c viven aqui en el codigo de la "pantalla":
 *   SIGUSR1 -> acceso a un puntero nulo: el proceso entero muere, y con el el control.
 *   SIGUSR2 -> bucle infinito: el lazo nunca vuelve al control.
 * La planta se queda con el ultimo comando recibido (como salidas PWM que quedan fijas).
 *
 * Uso: mono_vent -h ip_host [-u puerto] [-s seg] [-p prio] [-P p_insp] [-e peep] [-r rr]
 */
#include <arpa/inet.h>
#include <getopt.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/dispatch.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bg_proto.h"
#include "plant.h"
#include "rt_util.h"
#include "vent.h"

#define PROG "mono_vent"
#define PULSE_TICK (_PULSE_CODE_MINAVAIL + 0)

static volatile sig_atomic_t bug, stop;
static void on_sig(int sig)
{
    if (sig == SIGUSR1) bug = 1;
    if (sig == SIGUSR2) bug = 2;
    if (sig == SIGTERM) stop = 1;
}

/* "Pantalla": lee signos vitales y manda una linea UDP con el mismo formato que hmi.c (los campos de
 * seguridad y energia van en cero: el monolito no los tiene). */
static void hmi_task(int plant, int sock, const struct sockaddr_in *dst, const sensors_t *s, unsigned long n)
{
    static bg_vitals_t vt;
    if (bug == 1) { /* bug de la demo */
        volatile int *p = NULL;
        *p = 42;
    }
    if (bug == 2) {
        volatile unsigned long spin = 0;
        for (;;) spin++;
    }
    if (n % 100 == 0) {
        bg_hdr_t rv = { .type = BG_MSG_VITALS };
        MsgSend(plant, &rv, sizeof rv, &vt, sizeof vt);
    }
    char line[512];
    int len = snprintf(line, sizeof line,
                       "%.4f,%.3f,%.4f,%.4f,%.2f,%.1f,%.2f,%d,0,0,0,0,MONO,0,0,0,%d,%.2f,%.4f\n", rt_now_s(),
                       s->paw, s->flow, vt.have ? vt.spo2 : -1.0, vt.etco2, vt.hr, vt.age_s, vt.src, (int)getpid(),
                       vt.r, vt.c * 1e3);
    sendto(sock, line, (size_t)len, 0, (const struct sockaddr *)dst, sizeof *dst);
}

int main(int argc, char **argv)
{
    const char *host = NULL;
    int port = 5005, prio = 50, opt;
    double secs = 3600;
    vent_settings_t set = { .rr = 12, .ie = 2.0, .p_insp = 20, .peep = 5, .rise_s = 0.1 };
    while ((opt = getopt(argc, argv, "h:u:s:p:P:e:r:")) != -1) {
        switch (opt) {
        case 'h': host = optarg; break;
        case 'u': port = atoi(optarg); break;
        case 's': secs = atof(optarg); break;
        case 'p': prio = atoi(optarg); break;
        case 'P': set.p_insp = atof(optarg); break;
        case 'e': set.peep = atof(optarg); break;
        case 'r': set.rr = atof(optarg); break;
        default:
            fprintf(stderr, "uso: %s -h ip_host [-u puerto] [-s seg] [-p prio] [-P p_insp] [-e peep] [-r rr]\n",
                    PROG);
            return 1;
        }
    }
    if (!host) { fprintf(stderr, "%s: falta -h ip_host\n", PROG); return 1; }
    signal(SIGUSR1, on_sig);
    signal(SIGUSR2, on_sig);
    signal(SIGTERM, on_sig);

    int plant = -1;
    for (int i = 0; i < 500 && plant == -1; i++) {
        plant = name_open(BG_PNEUMATIC_NAME, 0);
        if (plant == -1) usleep(10000);
    }
    if (plant == -1) rt_die(PROG, "name_open(" BG_PNEUMATIC_NAME ")");

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == -1) rt_die(PROG, "socket");
    struct sockaddr_in dst = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (inet_pton(AF_INET, host, &dst.sin_addr) != 1) { fprintf(stderr, "%s: ip invalida\n", PROG); return 1; }

    rt_setup(PROG, prio);
    int chid = ChannelCreate(_NTO_CHF_PRIVATE);
    if (chid == -1) rt_die(PROG, "ChannelCreate");
    timer_t tm = rt_periodic_pulse(PROG, chid, prio, PULSE_TICK, 1000000);

    vent_t vent;
    double t_start = rt_now_s(), last = t_start;
    vent_init_pcv(&vent, &set, plant_default_params().pbl_max, t_start);
    printf("%s: pid %d, PCV %.0f/%.0f cmH2O, %.0f rpm, prio %d, un solo proceso (control + pantalla)\n", PROG,
           getpid(), set.p_insp, set.peep, set.rr, prio);
    fflush(stdout);

    bg_hdr_t rd = { .type = BG_MSG_READ };
    bg_write_msg_t wr = { .hdr = { .type = BG_MSG_WRITE } };
    struct _pulse pulse;
    unsigned long n = 0;
    while (!stop && rt_now_s() - t_start < secs) {
        if (MsgReceivePulse(chid, &pulse, sizeof pulse, NULL) == -1) {
            if (errno == EINTR) continue;
            rt_die(PROG, "MsgReceivePulse");
        }
        double now = rt_now_s(), dt = now - last;
        last = now;
        sensors_t s;
        if (MsgSend(plant, &rd, sizeof rd, &s, sizeof s) == -1) break;
        vent_step(&vent, now, dt, &s, &wr.act);
        if (MsgSend(plant, &wr, sizeof wr, NULL, 0) == -1) break;
        if (n % 10 == 0) hmi_task(plant, sock, &dst, &s, n); /* la pantalla, en el mismo lazo */
        n++;
    }
    timer_delete(tm);
    rt_relax();
    printf("%s: %lu ciclos\n", PROG, n);
    return 0;
}
