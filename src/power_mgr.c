/* power_mgr.c - M5: gestión de energía del ventilador, proceso QNX (prio 30).
 *
 * Cada 1 ms (timer + pulso) "muestrea" la tensión de red: si cae por debajo de 18 V durante 2 muestras,
 * declara el corte. Cada 10 ms lee los sensores de la planta para calcular la potencia del soplador y
 * avanza el modelo de batería (battery.c). Con el corte:
 *   - recorta lo no esencial (humidificador apagado, pantalla atenuada; la telemetría sigue),
 *   - avisa a safety_mon con un pulso (BG_PULSE_POWER) y estima la autonomía.
 * Con -x el tiempo de la batería corre más rápido (p. ej. -x 60: 1 s real = 1 min de batería), para que la
 * demo muestre una descarga en minutos. El ventilador y las alarmas siguen en tiempo real.
 *
 * Uso: power_mgr [-s seg] [-p prio] [-x escala] [-b soc_inicial] [-o salida.csv]
 */
#include <getopt.h>
#include <sys/dispatch.h>
#include <unistd.h>

#include "battery.h"
#include "bg_power.h"
#include "bg_proto.h"
#include "rt_util.h"

#define PROG "power_mgr"
#define PULSE_TICK (_PULSE_CODE_MINAVAIL + 0)
#define MAINS_OK_V 24.0
#define MAINS_MIN_V 18.0
#define V_CUTOFF 21.0

typedef union {
    bg_hdr_t hdr;
    struct _pulse pulse;
    bg_mains_msg_t mains;
} pwr_msg_t;

typedef struct {
    double t;
    float soc, v, i, p, autonomy_min;
    uint8_t bits;
} logrec_t;

int main(int argc, char **argv)
{
    double secs = 60, scale = 1, soc0 = 1.0;
    int prio = 30, opt;
    const char *out_path = NULL;
    while ((opt = getopt(argc, argv, "s:p:x:b:o:")) != -1) {
        switch (opt) {
        case 's': secs = atof(optarg); break;
        case 'p': prio = atoi(optarg); break;
        case 'x': scale = atof(optarg); break;
        case 'b': soc0 = atof(optarg); break;
        case 'o': out_path = optarg; break;
        default:
            fprintf(stderr, "uso: %s [-s seg] [-p prio] [-x escala] [-b soc_inicial] [-o salida.csv]\n", PROG);
            return 1;
        }
    }
    size_t cap = (size_t)(secs * 10) + 100, n = 0;
    logrec_t *log = calloc(cap, sizeof *log);
    if (!log) rt_die(PROG, "calloc");

    name_attach_t *att = name_attach(NULL, BG_POWER_NAME, 0);
    if (!att) rt_die(PROG, "name_attach");
    int plant = -1, safety = -1;
    for (int i = 0; i < 500 && plant == -1; i++) {
        plant = name_open(BG_PNEUMATIC_NAME, 0);
        if (plant == -1) usleep(10000);
    }
    if (plant == -1) rt_die(PROG, "name_open(" BG_PNEUMATIC_NAME ")");

    rt_setup(PROG, prio);
    timer_t tm = rt_periodic_pulse(PROG, att->chid, prio, PULSE_TICK, 1000000);

    battery_params_t bp = battery_default_params();
    battery_t bat;
    battery_init(&bat, &bp, soc0);
    loads_t loads = loads_default();
    double mains_v = MAINS_OK_V; /* lo que "mide" el ADC de la entrada de red */
    int mains_cmd = 1, low_count = 0, on_battery = 0;
    uint32_t bits = 0, sent_bits = ~0u;
    double t_start = rt_now_s(), last = t_start, last_log = t_start, t_mains_cmd = 0;
    double p_avg = 0, e_acc = 0, t_acc = 0;
    unsigned long ticks = 0;
    bg_hdr_t rd = { .type = BG_MSG_READ };
    pwr_msg_t m;
    printf("%s: batería %.0f %%, escala de tiempo x%.0f, prio %d\n", PROG, soc0 * 100, scale, prio);
    fflush(stdout);

    for (;;) {
        int rcvid = MsgReceive(att->chid, &m, sizeof m, NULL);
        if (rcvid == -1) rt_die(PROG, "MsgReceive");
        double now = rt_now_s();
        if (rcvid > 0) {
            if (m.hdr.type == _IO_CONNECT) {
                MsgReply(rcvid, EOK, NULL, 0);
            } else if (m.hdr.type == BG_MSG_MAINS) {
                mains_cmd = m.mains.on;
                t_mains_cmd = now;
                printf("%s: red %s t_abs=%.6f\n", PROG, mains_cmd ? "restablecida" : "CORTADA", now);
                fflush(stdout);
                MsgReply(rcvid, EOK, NULL, 0);
            } else if (m.hdr.type == BG_MSG_POWER_STATUS) {
                bg_power_status_t st = { bits, bat.soc, bat.v, bat.i, p_avg,
                                         p_avg > 0 ? battery_energy_wh(&bat, 0.0) * 3600 / p_avg : 0 };
                MsgReply(rcvid, EOK, &st, sizeof st);
            } else {
                MsgError(rcvid, ENOSYS);
            }
            continue;
        }
        if (m.pulse.code == _PULSE_CODE_DISCONNECT) { ConnectDetach(m.pulse.scoid); continue; }
        if (m.pulse.code != PULSE_TICK) continue;
        if (now - t_start >= secs) break;
        ticks++;

        /* ADC de la entrada de red: la tensión cae con el corte (con una pequeña constante de tiempo de los
         * condensadores de la fuente: unos 3 ms). */
        double target_v = mains_cmd ? MAINS_OK_V : 0.0;
        mains_v += (target_v - mains_v) * 0.3;
        low_count = mains_v < MAINS_MIN_V ? low_count + 1 : 0;
        int mains_ok = low_count < 2;
        if (!mains_ok && !on_battery) {
            on_battery = 1;
            printf("%s: CORTE detectado a %.1f ms de la orden; recorte de consumos\n", PROG,
                   (now - t_mains_cmd) * 1e3);
            fflush(stdout);
        } else if (mains_ok && on_battery && mains_v > 0.95 * MAINS_OK_V) {
            on_battery = 0;
            printf("%s: red restablecida; consumos normales y carga\n", PROG);
            fflush(stdout);
        }

        if (ticks % 10 == 0) { /* cada 10 ms: potencia y batería */
            sensors_t s;
            if (MsgSend(plant, &rd, sizeof rd, &s, sizeof s) == -1) break;
            double dt = now - last;
            last = now;
            double p = loads_power(&loads, s.paw, s.flow, on_battery);
            battery_step(&bat, p, !on_battery, dt * scale);
            e_acc += p * dt;
            t_acc += dt;
            if (t_acc >= 10.0 || p_avg == 0) { /* potencia media en ventanas de 10 s (dos respiraciones) */
                p_avg = e_acc / t_acc;
                e_acc = t_acc = 0;
            }
            double autonomy = p_avg > 0 ? battery_energy_wh(&bat, 0.0) * 3600 / p_avg : 0; /* s de batería */
            bits = (on_battery ? PWR_ON_BATTERY | PWR_SHEDDING : 0);
            if (on_battery && (bat.soc < 0.20 || autonomy < 1800)) bits |= PWR_BATTERY_LOW;
            if (on_battery && (bat.soc < 0.05 || bat.v < V_CUTOFF + 0.5)) bits |= PWR_BATTERY_CRIT;

            if (bits != sent_bits) { /* avisar a safety_mon de cada cambio */
                if (safety == -1) safety = name_open(BG_SAFETY_NAME, 0);
                if (safety != -1) MsgSendPulse(safety, prio, BG_PULSE_POWER, (int)bits);
                sent_bits = bits;
            }
            if (now - last_log >= 0.1 && n < cap) {
                last_log = now;
                logrec_t *r = &log[n++];
                r->t = now;
                r->soc = (float)bat.soc;
                r->v = (float)bat.v;
                r->i = (float)bat.i;
                r->p = (float)p_avg;
                r->autonomy_min = (float)(autonomy / 60);
                r->bits = (uint8_t)bits;
            }
        }
    }
    timer_delete(tm);
    if (safety != -1) name_close(safety);
    name_detach(att, 0);
    rt_relax();
    printf("%s: final SoC %.1f %%, %.2f V, potencia media %.1f W\n", PROG, bat.soc * 100, bat.v, p_avg);
    if (out_path) {
        FILE *f = fopen(out_path, "w");
        if (!f) rt_die(PROG, out_path);
        fprintf(f, "t_s,soc,v_batt,i_batt,p_avg_w,autonomia_min,bits\n");
        for (size_t i = 0; i < n; i++)
            fprintf(f, "%.3f,%.4f,%.3f,%.3f,%.2f,%.1f,%u\n", log[i].t, log[i].soc, log[i].v, log[i].i,
                    log[i].p, log[i].autonomy_min, log[i].bits);
        fclose(f);
    }
    return 0;
}
