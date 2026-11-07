/* safety_mon.c - M4: monitor de seguridad y alarmas, proceso QNX independiente (prio 55).
 *
 * No confía en breath_ctrl: lee los sensores por su cuenta y vigila al controlador de dos formas.
 *   1. MUERTE: breath_ctrl abre una conexión a nuestro canal para mandar latidos. Si su proceso muere,
 *      el kernel de QNX cierra esa conexión y nos manda un pulso _PULSE_CODE_DISCONNECT. Detección
 *      inmediata, sin plazos ni sondeo.
 *   2. CUELGUE: los latidos (pulsos con un contador) alimentan el watchdog de la tarea 1. Si deja de
 *      latir, se lo mata y se sigue como en 1.
 * Ante una falla del controlador:
 *   - OVERRIDE a la planta: desde ese momento la planta ignora a breath_ctrl y obedece a safety_mon,
 *     que ventila con un PCV de respaldo (el mismo vent.c, con los ajustes de la primera vez).
 *   - Relanza breath_ctrl con posix_spawn(), con el mismo inicio de ciclo (-T): ambos respiran en fase.
 *   - Cuando el nuevo controlador lleva HANDBACK_BEATS latidos seguidos, le devuelve el mando al
 *     empezar una respiración.
 * Alarmas por sensores (alarms.c): presión alta (con alivio de presión: válvula espiratoria abierta el
 * resto de la respiración), desconexión, apnea; y de volumen (tarea 2, vt_meter) si está completa.
 *
 * HMI: con -H <ip_host>, lanza también el proceso hmi (prio 10) y lo relanza si muere. Una HMI caída no
 * cambia nada de la ventilación: solo se la vuelve a lanzar.
 *
 * Uso: safety_mon [-s seg] [-p prio] [-m pcv|vcv] [-P p_insp] [-V vt_ml] [-e peep] [-r rr]
 *                 [-c ruta_breath_ctrl] [-H ip_host] [-o eventos.csv]
 */
#include <getopt.h>
#include <signal.h>
#include <spawn.h>
#include <sys/dispatch.h>
#include <unistd.h>

#include "alarms.h"
#include "bg_power.h"
#include "bg_proto.h"
#include "plant.h"
#include "rt_util.h"
#include "vent.h"
#include "vt_meter.h"
#include "watchdog.h"

#define PROG "safety_mon"
#define PULSE_TICK (_PULSE_CODE_MINAVAIL + 0)
#define WD_TIMEOUT_S 0.200   /* > huecos de la VM (hasta ~45 ms) + periodo del latido (10 ms) */
#define WD_STARTUP_S 3.0
#define HANDBACK_BEATS 50    /* ~0.5 s de latidos seguidos antes de devolver el mando */

typedef union {
    bg_hdr_t hdr;
    struct _pulse pulse;
} safety_msg_t;

typedef struct {
    double t;
    char what[24];
    char detail[64];
} event_t;

static event_t events[4096];
static size_t n_events;
static double t_start;

static void event(double t, const char *what, const char *detail)
{
    if (n_events < sizeof events / sizeof events[0]) {
        event_t *e = &events[n_events++];
        e->t = t;
        snprintf(e->what, sizeof e->what, "%s", what);
        snprintf(e->detail, sizeof e->detail, "%s", detail);
    }
    /* No se imprime aquí: printf es E/S y bloquearía el camino crítico. Se imprime todo al final. */
}

/* --- Lanzar breath_ctrl ------------------------------------------------------------------------ */
static const char *ctrl_path = "./breath_ctrl";
static const char *hmi_host = NULL; /* con -H: lanzar ./hmi apuntando a este host */

static pid_t spawn_hmi(double secs_left)
{
    char ss[32];
    snprintf(ss, sizeof ss, "%.3f", secs_left);
    char *argv[] = { "./hmi", "-h", (char *)hmi_host, "-s", ss, NULL };
    pid_t pid;
    extern char **environ;
    int rc = posix_spawn(&pid, "./hmi", NULL, NULL, argv, environ);
    if (rc != 0) { errno = rc; return -1; }
    return pid;
}
static char ctrl_args[8][32];
static int n_ctrl_args;

static pid_t spawn_ctrl(double t0_breath, double secs_left)
{
    char t0s[32], ss[32];
    snprintf(t0s, sizeof t0s, "%.6f", t0_breath);
    snprintf(ss, sizeof ss, "%.3f", secs_left);
    char *argv[24];
    int k = 0;
    argv[k++] = (char *)ctrl_path;
    for (int i = 0; i < n_ctrl_args; i++) argv[k++] = ctrl_args[i];
    argv[k++] = "-T"; argv[k++] = t0s;
    argv[k++] = "-s"; argv[k++] = ss;
    argv[k++] = "-S"; /* manda latidos a safety_mon */
    argv[k] = NULL;
    pid_t pid;
    extern char **environ;
    int rc = posix_spawn(&pid, ctrl_path, NULL, NULL, argv, environ);
    if (rc != 0) { errno = rc; rt_die(PROG, "posix_spawn(breath_ctrl)"); }
    return pid;
}

int main(int argc, char **argv)
{
    double secs = 60, vt_ml = 500;
    int prio = 55, opt, vcv = 0;
    const char *out_path = NULL;
    vent_settings_t set = { .rr = 12, .ie = 2.0, .p_insp = 20, .peep = 5, .rise_s = 0.1 };
    while ((opt = getopt(argc, argv, "s:p:m:P:V:e:r:c:H:o:")) != -1) {
        switch (opt) {
        case 's': secs = atof(optarg); break;
        case 'p': prio = atoi(optarg); break;
        case 'm': vcv = optarg[0] == 'v'; break;
        case 'P': set.p_insp = atof(optarg); break;
        case 'V': vt_ml = atof(optarg); break;
        case 'e': set.peep = atof(optarg); break;
        case 'r': set.rr = atof(optarg); break;
        case 'c': ctrl_path = optarg; break;
        case 'H': hmi_host = optarg; break;
        case 'o': out_path = optarg; break;
        default:
            fprintf(stderr, "uso: %s [-s seg] [-p prio] [-m pcv|vcv] [-P p_insp] [-V vt_ml] [-e peep] [-r rr] "
                            "[-c breath_ctrl] [-o eventos.csv]\n", PROG);
            return 1;
        }
    }
    /* Argumentos con que se lanza (y relanza) breath_ctrl. */
    snprintf(ctrl_args[n_ctrl_args++], 32, "-m");
    snprintf(ctrl_args[n_ctrl_args++], 32, "%s", vcv ? "vcv" : "pcv");
    snprintf(ctrl_args[n_ctrl_args++], 32, "%s", vcv ? "-V" : "-P");
    snprintf(ctrl_args[n_ctrl_args++], 32, "%.1f", vcv ? vt_ml : set.p_insp);
    snprintf(ctrl_args[n_ctrl_args++], 32, "-e");
    snprintf(ctrl_args[n_ctrl_args++], 32, "%.1f", set.peep);
    snprintf(ctrl_args[n_ctrl_args++], 32, "-r");
    snprintf(ctrl_args[n_ctrl_args++], 32, "%.1f", set.rr);

    signal(SIGCHLD, SIG_IGN); /* los hijos muertos no quedan como zombis */

    /* Nuestro canal, con nombre: aquí llegan los latidos y el aviso de desconexión. */
    name_attach_t *att = name_attach(NULL, BG_SAFETY_NAME, 0);
    if (!att) rt_die(PROG, "name_attach");

    int plant = -1;
    for (int i = 0; i < 500 && plant == -1; i++) {
        plant = name_open(BG_PNEUMATIC_NAME, 0);
        if (plant == -1) usleep(10000);
    }
    if (plant == -1) rt_die(PROG, "name_open(" BG_PNEUMATIC_NAME ")");

    rt_setup(PROG, prio);
    timer_t tm = rt_periodic_pulse(PROG, att->chid, prio, PULSE_TICK, 1000000);

    t_start = rt_now_s();
    const double period = 60.0 / set.rr, ti = period / (1.0 + set.ie);
    const double t0_breath = t_start + 0.5; /* inicio del ciclo respiratorio, común a todos */

    /* Respaldo: el mismo modo y ajustes, con el mismo inicio de ciclo. */
    vent_t backup;
    double pbl_max = plant_default_params().pbl_max;
    if (vcv) vent_init_vcv(&backup, &set, vt_ml / 1e3, 40.0, pbl_max, t0_breath);
    else vent_init_pcv(&backup, &set, pbl_max, t0_breath);

    alarm_cfg_t acfg = alarms_default_cfg(set.peep);
    alarms_t al;
    alarms_init(&al, &acfg, t_start);
    vt_meter_t vtm;
    vtm_init(&vtm);
    uint32_t vtm_breaths_seen = 0;

    watchdog_t wd;
    pid_t ctrl_pid = spawn_ctrl(t0_breath, secs - 0.5);
    wd_arm(&wd, (uint64_t)(rt_now_s() * 1e9), (uint64_t)(WD_TIMEOUT_S * 1e9), (uint64_t)(WD_STARTUP_S * 1e9));
    char buf[64];
    int ctrl_scoid = -1;
    int hmi_scoid = -1;
    pid_t hmi_pid = hmi_host ? spawn_hmi(secs - 0.5) : -1;
    uint32_t hmi_restarts = 0;
    double last_hmi_query = t_start;
    if (hmi_host) {
        snprintf(buf, sizeof buf, "pid %d", hmi_pid);
        event(t_start, "LANZA_HMI", buf);
    }
    int watching = 1; /* 0 tras la despedida del controlador */
    snprintf(buf, sizeof buf, "pid %d", ctrl_pid);
    event(t_start, "LANZA_CONTROL", buf);

    int overriding = 0;      /* 1 = la planta obedece a safety_mon */
    int backup_mode = 0;     /* 1 = el controlador falló y ventila el respaldo */
    int relief = 0;          /* 1 = alivio de presión hasta el fin de esta respiración */
    uint32_t relief_breath = 0, beats_since_spawn = 0, failovers = 0;
    unsigned long ticks = 0;
    int need_spawn = 0;
    uint32_t power_bits = 0;
    double last_tick = t_start;

    bg_hdr_t rd = { .type = BG_MSG_READ };
    bg_override_msg_t ov = { .hdr = { .type = BG_MSG_OVERRIDE } };
    safety_msg_t m;
    struct _msg_info info;

    for (;;) {
        int rcvid = MsgReceive(att->chid, &m, sizeof m, &info);
        if (rcvid == -1) rt_die(PROG, "MsgReceive");
        double now = rt_now_s();

        if (rcvid > 0) { /* mensajes: conexiones y consultas de estado de la HMI */
            if (m.hdr.type == _IO_CONNECT) MsgReply(rcvid, EOK, NULL, 0);
            else if (m.hdr.type == BG_MSG_SAFETY_STATUS) {
                hmi_scoid = info.scoid; /* así reconocemos la desconexión de la HMI */
                last_hmi_query = now;
                bg_safety_status_t st = { .alarms = al.active, .backup = backup_mode, .failovers = failovers,
                                          .hmi_restarts = hmi_restarts, .n_events = (uint32_t)n_events };
                if (n_events)
                    snprintf(st.last_event, sizeof st.last_event, "%s", events[n_events - 1].what);
                MsgReply(rcvid, EOK, &st, sizeof st);
            }
            else MsgError(rcvid, ENOSYS);
            continue;
        }

        int ctrl_failed = 0;
        const char *why = NULL;
        if (m.pulse.code == BG_PULSE_HEARTBEAT) {
            ctrl_scoid = m.pulse.scoid; /* así reconocemos su desconexión */
            wd_beat(&wd, (uint64_t)(now * 1e9), (uint32_t)m.pulse.value.sival_int);
            beats_since_spawn++;
            continue;
        }
        if (m.pulse.code == BG_PULSE_POWER) { /* avisos de power_mgr: alarmas de energía */
            uint32_t pb = (uint32_t)m.pulse.value.sival_int, chg = pb ^ power_bits;
            if (chg & PWR_ON_BATTERY)
                event(now, pb & PWR_ON_BATTERY ? "ENERGIA_CORTE" : "ENERGIA_RED",
                      pb & PWR_ON_BATTERY ? "en batería; consumos no esenciales recortados" : "red restablecida");
            if ((chg & PWR_BATTERY_LOW) && (pb & PWR_BATTERY_LOW)) event(now, "BATERIA_BAJA", "SoC < 20 % o < 30 min");
            if ((chg & PWR_BATTERY_CRIT) && (pb & PWR_BATTERY_CRIT)) event(now, "BATERIA_CRITICA", "SoC < 5 %");
            power_bits = pb;
            if (pb & (PWR_ON_BATTERY | PWR_BATTERY_LOW | PWR_BATTERY_CRIT)) al.active |= ALARM_POWER;
            else al.active &= ~ALARM_POWER;
            continue;
        }
        if (m.pulse.code == BG_PULSE_BYE) {
            if (m.pulse.scoid == ctrl_scoid) ctrl_scoid = -1; /* salida a propósito */
            watching = 0;
            event(now, "CONTROL_TERMINA", "despedida");
            continue;
        }
        if (m.pulse.code == _PULSE_CODE_DISCONNECT) {
            if (m.pulse.scoid == ctrl_scoid) { ctrl_failed = 1; why = "CONTROL_MUERTO"; ctrl_scoid = -1; }
            if (m.pulse.scoid == hmi_scoid && hmi_host && now - t_start < secs - 1.0) {
                /* La HMI murió: no afecta a la ventilación; solo se relanza. */
                hmi_scoid = -1;
                hmi_restarts++;
                hmi_pid = spawn_hmi(secs - (now - t_start));
                snprintf(buf, sizeof buf, "pid %d", hmi_pid);
                event(now, "HMI_MUERTA_RELANZADA", buf);
            }
            ConnectDetach(m.pulse.scoid);
        }
        if (m.pulse.code == PULSE_TICK) {
            ticks++;
            if (now - t_start >= secs) break;
            /* HMI colgada: deja de preguntar. Se la mata; su desconexión la relanza (abajo). */
            if (hmi_scoid != -1 && hmi_pid > 0 && now - last_hmi_query > 2.0) {
                snprintf(buf, sizeof buf, "pid %d sin consultas en %.1f s", hmi_pid, now - last_hmi_query);
                event(now, "HMI_COLGADA", buf);
                kill(hmi_pid, SIGKILL);
                last_hmi_query = now;
            }
            if (watching && wd_check(&wd, (uint64_t)(now * 1e9)) == WD_TIMEOUT) {
                ctrl_failed = 1;
                why = "CONTROL_COLGADO";
                kill(ctrl_pid, SIGKILL);
                ctrl_scoid = -1; /* su aviso de desconexión llegará después: no es otra falla */
            }
        }

        if (ctrl_failed) {
            if (!backup_mode) failovers++;
            backup_mode = 1;
            overriding = 1;
            event(now, why, "respaldo activo");
            need_spawn = 1; /* se relanza DESPUÉS de tomar el mando (abajo) */
        }
        if (m.pulse.code != PULSE_TICK && !ctrl_failed) continue;

        /* --- Cada tick: sensores propios, alarmas, respaldo --- */
        sensors_t s;
        if (MsgSend(plant, &rd, sizeof rd, &s, sizeof s) == -1) break; /* la planta terminó */

        double since = now - t0_breath;
        uint32_t breath = since >= 0 ? (uint32_t)(since / period) : 0;
        int insp = since >= 0 && since - breath * period < ti;

        uint32_t up = alarms_update(&al, now, s.paw, s.flow);
        vtm_update(&vtm, now, s.flow, insp);
        if (vtm.breaths != vtm_breaths_seen) { /* respiración completa medida (tarea 2) */
            vtm_breaths_seen = vtm.breaths;
            double lo = vcv ? 0.8 * vt_ml / 1e3 : 0.2, hi = vcv ? 1.2 * vt_ml / 1e3 : 1.0;
            uint32_t vt_al = 0;
            if (vtm.vti < lo) vt_al |= ALARM_LOW_VT;
            if (vtm.vti > hi) vt_al |= ALARM_HIGH_VT;
            if (vtm.vti > 0.1 && vtm.vte < 0.8 * vtm.vti) vt_al |= ALARM_LEAK;
            uint32_t vt_bits = ALARM_LOW_VT | ALARM_HIGH_VT | ALARM_LEAK;
            up |= vt_al & ~al.active;
            al.active = (al.active & ~vt_bits) | vt_al;
        }
        for (int b = 0; b < ALARM_COUNT; b++) {
            if (up & (1u << b)) {
                snprintf(buf, sizeof buf, "Paw %.2f flujo %.3f Vti %.0f Vte %.0f", s.paw, s.flow,
                         vtm.vti * 1e3, vtm.vte * 1e3);
                event(now, alarm_name(1u << b), buf);
            }
        }
        /* Defensa en profundidad: si el controlador vive pero no ventila (p. ej. congelado y sin
         * watchdog), la apnea detectada por los sensores también activa el respaldo. */
        /* Con el tubo desconectado la apnea es del circuito, no del controlador: no se lo mata. */
        if ((up & ALARM_APNEA) && !backup_mode && !(al.active & ALARM_DISCONNECT)) {
            backup_mode = 1;
            failovers++;
            kill(ctrl_pid, SIGKILL);
            ctrl_scoid = -1;
            event(now, "APNEA_CON_CONTROL_VIVO", "se mata y se relanza; respaldo activo");
            need_spawn = 1;
        }
        if (alarms_relief(&al) && !relief) {
            relief = 1;
            relief_breath = breath;
            overriding = 1;
            event(now, "ALIVIO_PRESION", "válvula espiratoria abierta hasta la próxima respiración");
        }
        if (relief && breath != relief_breath) relief = 0;

        /* ¿Devolver el mando? Controlador nuevo latiendo, al empezar una respiración. */
        if (backup_mode && beats_since_spawn >= HANDBACK_BEATS && insp && since - breath * period < 0.005) {
            backup_mode = 0;
            snprintf(buf, sizeof buf, "%u latidos del nuevo controlador", beats_since_spawn);
            event(now, "DEVUELVE_MANDO", buf);
        }

        actuators_t act;
        vent_step(&backup, now, now - last_tick, &s, &act); /* siempre corre: así su integral está listo */
        last_tick = now;
        if (relief) {
            act.blower = set.peep / pbl_max;
            act.exh_open = 1.0;
            act.peep_cmd = set.peep;
        }
        int want = backup_mode || relief;
        if (want || overriding) {
            ov.on = want;
            ov.act = act;
            if (MsgSend(plant, &ov, sizeof ov, NULL, 0) == -1) break;
            overriding = want;
        }
        if (need_spawn) { /* el respaldo ya manda: ahora sí, lo lento (cargar el binario desde disco) */
            need_spawn = 0;
            double t_ov = rt_now_s();
            ctrl_pid = spawn_ctrl(t0_breath, secs - (t_ov - t_start));
            wd_arm(&wd, (uint64_t)(rt_now_s() * 1e9), (uint64_t)(WD_TIMEOUT_S * 1e9), (uint64_t)(WD_STARTUP_S * 1e9));
            beats_since_spawn = 0;
            watching = 1;
            snprintf(buf, sizeof buf, "pid %d; override enviado %.2f ms tras detectar; spawn %.1f ms", ctrl_pid,
                     (t_ov - now) * 1e3, (rt_now_s() - t_ov) * 1e3);
            event(t_ov, "RELANZA_CONTROL", buf);
        }
    }

    timer_delete(tm);
    if (ctrl_pid > 0) kill(ctrl_pid, SIGTERM);
    if (hmi_pid > 0) kill(hmi_pid, SIGTERM);
    name_detach(att, 0);
    rt_relax();
    for (size_t i = 0; i < n_events; i++)
        printf("%s: t=%.3f %s %s\n", PROG, events[i].t - t_start, events[i].what, events[i].detail);
    printf("%s: %lu ticks, %u fallas del controlador, %zu eventos; vt_meter: %u respiraciones, Vti %.0f, "
           "Vte %.0f mL\n", PROG, ticks, failovers, n_events, vtm.breaths, vtm.vti * 1e3, vtm.vte * 1e3);
    if (out_path) {
        FILE *f = fopen(out_path, "w");
        if (!f) rt_die(PROG, out_path);
        fprintf(f, "t_s,evento,detalle\n");
        for (size_t i = 0; i < n_events; i++)
            fprintf(f, "%.6f,%s,%s\n", events[i].t, events[i].what, events[i].detail);
        fclose(f);
    }
    return 0;
}
