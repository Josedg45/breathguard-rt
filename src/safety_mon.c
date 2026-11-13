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
 * Mecanica (lung_mon.c): auto-PEEP estimado en cada respiracion, con alarma (-A cmH2O). Volumen corriente
 * protector: el limite de Vt alto es 8 mL/kg de peso ideal, calculado con la talla (-h cm) y el sexo (-g m|f).
 * Metricas de tiempo real: breath_ctrl las manda en cada consulta de ajustes; se reenvian a la HMI.
 *
 * Copiloto (punto 5, -K): lanza ./copilot (prio 8, tipo "copilot") y lo relanza si muere. En cada tick mide la
 * respiración (breath_feat); al cerrarla responde la consulta diferida del copiloto. Sus sugerencias pasan por
 * cp_check (con las mediciones propias de safety_mon) y solo se aplican si el operador las confirma (COPILOT_ACK).
 *
 * Canal con _NTO_CHF_FIXED_PRIORITY: por defecto QNX baja el hilo que recibe a la prioridad del cliente (medido:
 * 55 -> 8 al atender al copiloto). safety_mon nunca debe correr a la prioridad de un cliente de baja prioridad.
 *
 * Uso: safety_mon [-s seg] [-p prio] [-m pcv|vcv] [-P p_insp] [-V vt_ml] [-e peep] [-r rr]
 *                 [-h talla_cm] [-g m|f] [-A auto_peep_max] [-c ruta_breath_ctrl] [-H ip_host] [-o eventos.csv]
 *                 [-K] [-Q copilot.csv] [-k ruta_copilot]
 */
#include <getopt.h>
#include <signal.h>
#include <spawn.h>
#include <secpol/secpol.h>
#include <sys/dispatch.h>
#include <unistd.h>

#include "alarms.h"
#include "breath_feat.h"
#include "copilot_logic.h"
#include "lung_mon.h"
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
    bg_settings_msg_t sset;
    bg_settings_get_t sget;
    bg_copilot_get_t cget;
    bg_copilot_suggest_t csug;
    bg_copilot_ack_t cack;
} safety_msg_t;

#define SEC_ALARM_S 10.0 /* la alarma de comando no autorizado dura esto tras el último rechazo */

typedef struct {
    double t;
    char what[24];
    char detail[64];
} event_t;

static volatile sig_atomic_t stop; /* SIGTERM: apagado ordenado (termina a sus hijos) */
static void on_term(int sig) { (void)sig; stop = 1; }

#define N_EVENTS 4096
static event_t events[N_EVENTS];
static size_t n_events;
static double t_start;

static void event(double t, const char *what, const char *detail)
{
    { /* anillo: sin límite de tiempo se conservan los últimos */
        event_t *e = &events[n_events++ % N_EVENTS];
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
    snprintf(ss, sizeof ss, "%.3f", rt_child_secs(secs_left));
    char *argv[] = { "./hmi", "-h", (char *)hmi_host, "-s", ss, NULL };
    pid_t pid;
    extern char **environ;
    /* Con política de seguridad (capa 2), la HMI nace con el tipo derivado "hmi" (p. ej. bg_hmi_t), que no puede
     * conectarse al canal de la planta. Sin política, no hace nada. */
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    secpol_posix_spawnattr_settypeid(NULL, &attr, "hmi", 0);
    int rc = posix_spawn(&pid, "./hmi", NULL, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) { errno = rc; return -1; }
    return pid;
}
/* Copiloto (punto 5): proceso de baja prioridad con el tipo derivado "copilot" (solo puede hablar con safety_mon). */
static const char *copilot_csv = NULL;
static const char *copilot_path = "./copilot";

static pid_t spawn_copilot(double secs_left)
{
    char ss[32];
    snprintf(ss, sizeof ss, "%.3f", rt_child_secs(secs_left));
    char *argv[] = { (char *)copilot_path, "-s", ss, NULL, NULL, NULL };
    if (copilot_csv) { argv[3] = "-o"; argv[4] = (char *)copilot_csv; }
    pid_t pid;
    extern char **environ;
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    secpol_posix_spawnattr_settypeid(NULL, &attr, "copilot", 0);
    int rc = posix_spawn(&pid, copilot_path, NULL, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) { errno = rc; return -1; }
    return pid;
}

static char ctrl_args[10][32];
static int n_ctrl_args;

/* breath_ctrl se lanza (y relanza) con los ajustes vigentes y su inicio de ciclo; si hay un cambio programado,
 * lo pide el mismo con BG_MSG_SETTINGS_GET al arrancar. */
static pid_t spawn_ctrl(int plant, const bg_settings_t *cur, double secs_left)
{
    char t0s[32], ss[32];
    n_ctrl_args = 0;
    snprintf(ctrl_args[n_ctrl_args++], 32, "-m");
    snprintf(ctrl_args[n_ctrl_args++], 32, "%s", cur->vcv ? "vcv" : "pcv");
    snprintf(ctrl_args[n_ctrl_args++], 32, "%s", cur->vcv ? "-V" : "-P");
    snprintf(ctrl_args[n_ctrl_args++], 32, "%.1f", cur->vcv ? cur->vt * 1e3 : cur->set.p_insp);
    snprintf(ctrl_args[n_ctrl_args++], 32, "-e");
    snprintf(ctrl_args[n_ctrl_args++], 32, "%.2f", cur->set.peep);
    snprintf(ctrl_args[n_ctrl_args++], 32, "-r");
    snprintf(ctrl_args[n_ctrl_args++], 32, "%.3f", cur->set.rr);
    snprintf(ctrl_args[n_ctrl_args++], 32, "-i");
    snprintf(ctrl_args[n_ctrl_args++], 32, "%.3f", cur->set.ie);
    snprintf(t0s, sizeof t0s, "%.6f", cur->t0);
    snprintf(ss, sizeof ss, "%.3f", rt_child_secs(secs_left));
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
    /* POSIX_SPAWN_HOLD (extensión de QNX): el hijo se crea detenido, antes de su primera instrucción. Así queda
     * autorizado en la planta antes de que pueda mandar su primer WRITE, sin carrera. Luego se suelta con SIGCONT. */
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    secpol_posix_spawnattr_settypeid(NULL, &attr, "ctrl", 0); /* tipo derivado (capa 2); sin política, nada */
    uint32_t xf = 0;
    posix_spawnattr_getxflags(&attr, &xf); /* sin perder la bandera de tipo que puso secpol */
    posix_spawnattr_setxflags(&attr, xf | POSIX_SPAWN_HOLD);
    int rc = posix_spawn(&pid, ctrl_path, NULL, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) { errno = rc; rt_die(PROG, "posix_spawn(breath_ctrl)"); }
    bg_auth_msg_t au = { .hdr = { .type = BG_MSG_AUTH }, .pid = pid };
    if (MsgSend(plant, &au, sizeof au, NULL, 0) == -1) { /* EPERM: otro proceso reclamó la planta antes */
        kill(pid, SIGKILL);
        rt_die(PROG, "MsgSend(AUTH): la planta no acepta a safety_mon");
    }
    kill(pid, SIGCONT);
    return pid;
}

/* Limites de Vt (L): en PCV, de 200 mL a 8 mL/kg de peso ideal (ventilacion protectora); en VCV, +-20 % del
 * Vt puesto, y nunca por encima de 8 mL/kg. */
static void vt_limits(const bg_settings_t *cur, double ibw, double *lo, double *hi)
{
    double prot = VT_PROTECT_HI_ML_KG * ibw / 1e3;
    *lo = cur->vcv ? 0.8 * cur->vt : 0.2;
    *hi = cur->vcv && 1.2 * cur->vt < prot ? 1.2 * cur->vt : prot;
}

static void init_vent(vent_t *v, const bg_settings_t *s, double pbl_max)
{
    if (s->vcv) vent_init_vcv(v, &s->set, s->vt, 40.0, pbl_max, s->t0);
    else vent_init_pcv(v, &s->set, pbl_max, s->t0);
}

int main(int argc, char **argv)
{
    double secs = 60, vt_ml = 500;
    int prio = 55, opt, vcv = 0;
    const char *out_path = NULL;
    vent_settings_t set = { .rr = 12, .ie = 2.0, .p_insp = 20, .peep = 5, .rise_s = 0.1 };
    double height_cm = 180.3; /* paciente StandardMale de Pulse: 71 in */
    int male = 1;
    lung_cfg_t lcfg = lung_default_cfg();
    int use_copilot = 0;
    while ((opt = getopt(argc, argv, "s:p:m:P:V:e:r:h:g:A:c:H:o:KQ:k:")) != -1) {
        switch (opt) {
        case 's': secs = atof(optarg); break;
        case 'p': prio = atoi(optarg); break;
        case 'm': vcv = optarg[0] == 'v'; break;
        case 'P': set.p_insp = atof(optarg); break;
        case 'V': vt_ml = atof(optarg); break;
        case 'e': set.peep = atof(optarg); break;
        case 'r': set.rr = atof(optarg); break;
        case 'h': height_cm = atof(optarg); break;
        case 'g': male = optarg[0] != 'f'; break;
        case 'A': lcfg.auto_peep_max = atof(optarg); break;
        case 'c': ctrl_path = optarg; break;
        case 'H': hmi_host = optarg; break;
        case 'o': out_path = optarg; break;
        case 'K': use_copilot = 1; break;
        case 'Q': copilot_csv = optarg; break;
        case 'k': copilot_path = optarg; break;
        default:
            fprintf(stderr, "uso: %s [-s seg] [-p prio] [-m pcv|vcv] [-P p_insp] [-V vt_ml] [-e peep] [-r rr] "
                            "[-h talla_cm] [-g m|f] [-A auto_peep_max] [-c breath_ctrl] [-H ip_host] [-o eventos.csv] "
                            "[-K] [-Q copilot.csv] [-k copilot]\n", PROG);
            return 1;
        }
    }
    secs = rt_duration(secs); /* -s 0: sin límite (arranque desde la imagen) */
    /* Ajustes vigentes (cur) y, si el operador pidio un cambio, los programados (next, desde next.t0). */
    bg_settings_t cur = { .seq = 0, .vcv = vcv, .set = set, .vt = vt_ml / 1e3 }, next = cur;
    int pending = 0;
    uint32_t breath_base = 0; /* respiraciones completas con calendarios anteriores */

    signal(SIGCHLD, SIG_IGN); /* los hijos muertos no quedan como zombis */
    signal(SIGTERM, on_term);

    /* Nuestro canal, con nombre: aquí llegan los latidos y el aviso de desconexión. Lo creamos nosotros (no
     * name_attach) para añadir _NTO_CHF_FIXED_PRIORITY a los flags que pone name_attach. */
    int chid = ChannelCreate(_NTO_CHF_FIXED_PRIORITY | _NTO_CHF_DISCONNECT | _NTO_CHF_COID_DISCONNECT |
                             _NTO_CHF_UNBLOCK);
    if (chid == -1) rt_die(PROG, "ChannelCreate");
    dispatch_t *dpp = dispatch_create_channel(chid, DISPATCH_FLAG_NOLOCK);
    if (!dpp) rt_die(PROG, "dispatch_create_channel");
    name_attach_t *att = name_attach(dpp, BG_SAFETY_NAME, 0);
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
    cur.t0 = t_start + 0.5; /* inicio del ciclo respiratorio, común a todos */

    /* Respaldo: el mismo modo y ajustes, con el mismo inicio de ciclo. */
    vent_t backup;
    double pbl_max = plant_default_params().pbl_max;
    init_vent(&backup, &cur, pbl_max);

    alarm_cfg_t acfg = alarms_default_cfg(set.peep);
    alarms_t al;
    alarms_init(&al, &acfg, t_start);
    vt_meter_t vtm;
    vtm_init(&vtm);
    lung_mon_t lm;
    lung_init(&lm, &lcfg);
    const double ibw = ibw_kg(height_cm, male);
    bg_loop_stats_t loop = { 0 };
    double loop_t = -1; /* cuando llegaron las ultimas metricas del lazo */
    uint32_t vtm_breaths_seen = 0;

    watchdog_t wd;
    pid_t ctrl_pid = spawn_ctrl(plant, &cur, secs - 0.5);
    wd_arm(&wd, (uint64_t)(rt_now_s() * 1e9), (uint64_t)(WD_TIMEOUT_S * 1e9), (uint64_t)(WD_STARTUP_S * 1e9));
    char buf[192]; /* event() recorta a 64 */
    int ctrl_scoid = -1;
    int hmi_scoid = -1;
    pid_t hmi_pid = hmi_host ? spawn_hmi(secs - 0.5) : -1;
    uint32_t hmi_restarts = 0;
    double last_hmi_query = t_start;
    if (hmi_host) {
        snprintf(buf, sizeof buf, "pid %d", hmi_pid);
        event(t_start, "LANZA_HMI", buf);
    }
    /* Copiloto */
    breath_feat_t bfm;
    bf_init(&bfm);
    int bf_dirty = 1;              /* la respiración en curso no sirve al copiloto (alarma, respaldo, cambio...) */
    cp_breath_t cp_ring[4];        /* las últimas respiraciones medidas por safety_mon */
    memset(cp_ring, 0, sizeof cp_ring);
    uint32_t cp_n = 0;             /* respiraciones cerradas */
    int cp_rcvid = -1;             /* consulta del copiloto retenida hasta la próxima respiración */
    cp_check_t cpc;
    cp_check_init(&cpc);
    bg_copilot_status_t cps = { .enabled = use_copilot, .pid = -1, .last_cls = -1 };
    int cp_scoid = -1;
    char cp_last_why[64] = "";
    if (use_copilot) {
        cps.pid = spawn_copilot(secs - 0.5);
        snprintf(buf, sizeof buf, "pid %d", cps.pid);
        event(t_start, "LANZA_COPILOTO", buf);
    }
    int bf_dirty_next = 0;
    bg_settings_t bf_set = cur; /* ajustes con que empezó la respiración en curso */
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

    bg_hdr_t rd = { .type = BG_MSG_READ }, rv = { .type = BG_MSG_VITALS };
    sensors_t last_s = { 0 };  /* la última lectura, para la HMI (BG_MSG_MONITOR) */
    bg_vitals_t last_vt = { 0 };
    uint32_t denied_seen = 0;  /* rechazos de la planta ya registrados como evento */
    double sec_alarm_t = -1;   /* último rechazo visto */
    bg_override_msg_t ov = { .hdr = { .type = BG_MSG_OVERRIDE } };
    safety_msg_t m;
    struct _msg_info info;

    for (;;) {
        int rcvid = MsgReceive(att->chid, &m, sizeof m, &info);
        if (rcvid == -1 && errno == EINTR) { /* una señal: ¿apagado? */
            if (stop) break;
            continue;
        }
        if (rcvid == -1) rt_die(PROG, "MsgReceive");
        double now = rt_now_s();

        if (rcvid > 0) { /* mensajes: conexiones y consultas de estado de la HMI */
            if (m.hdr.type == _IO_CONNECT) MsgReply(rcvid, EOK, NULL, 0);
            else if (m.hdr.type == BG_MSG_MONITOR) { /* la HMI ve la planta solo a través de safety_mon */
                hmi_scoid = info.scoid;
                last_hmi_query = now;
                bg_monitor_t mo = { .s = last_s, .vt = last_vt };
                MsgReply(rcvid, EOK, &mo, sizeof mo);
            }
            else if (m.hdr.type == BG_MSG_SAFETY_STATUS) {
                hmi_scoid = info.scoid; /* así reconocemos la desconexión de la HMI */
                last_hmi_query = now;
                bg_safety_status_t st = { .alarms = al.active, .backup = backup_mode, .failovers = failovers,
                                          .hmi_restarts = hmi_restarts, .n_events = (uint32_t)n_events };
                if (n_events)
                    snprintf(st.last_event, sizeof st.last_event, "%s", events[(n_events - 1) % N_EVENTS].what);
                st.set = pending ? next : cur;
                st.pending = pending;
                st.loop = loop;
                st.loop_age_s = loop_t < 0 ? -1 : now - loop_t;
                st.ibw_kg = ibw;
                double vlo, vhi;
                vt_limits(&cur, ibw, &vlo, &vhi);
                st.lim_p_high = al.cfg.p_alarm;
                st.lim_p_low = al.cfg.p_low;
                st.lim_apnea_s = al.cfg.apnea_s;
                st.lim_vt_lo = vlo;
                st.lim_vt_hi = vhi;
                st.lim_auto_peep = lcfg.auto_peep_max;
                st.auto_peep = lm.valid ? lm.auto_peep : -1;
                st.q_ee = lm.q_ee;
                st.tau_exp = lm.valid ? lm.tau : -1;
                st.eef = lm.eef;
                st.denied = last_vt.denied;
                cps.id = cpc.id;
                cps.state = cpc.state;
                cps.cls = cpc.cls;
                cps.action = cpc.action;
                cps.conf = cpc.conf;
                cps.age_s = cpc.id ? now - cpc.t : -1;
                cps.req = cpc.req;
                memcpy(cps.text, cpc.text, sizeof cps.text);
                memcpy(cps.why, cpc.why, sizeof cps.why);
                cps.n_suggest = cpc.n_suggest; cps.n_blocked = cpc.n_blocked; cps.n_proposed = cpc.n_proposed;
                cps.n_accepted = cpc.n_accepted; cps.n_rejected = cpc.n_rejected; cps.n_expired = cpc.n_expired;
                st.cp = cps;
                MsgReply(rcvid, EOK, &st, sizeof st);
            }
            else if (m.hdr.type == BG_MSG_SETTINGS_GET) { /* breath_ctrl: lo más reciente, con su t0 */
                if (info.srcmsglen >= (ssize_t)sizeof m.sget) { loop = m.sget.loop; loop_t = now; } /* sus metricas */
                MsgReply(rcvid, EOK, pending ? &next : &cur, sizeof cur);
            }
            else if (m.hdr.type == BG_MSG_SETTINGS_SET) { /* operador, vía HMI: validar y programar */
                const bg_settings_t *base = pending ? &next : &cur;
                bg_settings_t req = m.sset.req;
                bg_settings_reply_t r = { 0 };
                req.set.rise_s = cur.set.rise_s;
                /* Validate first: an invalid value is a final NAK, while "busy" (a pending change about to start,
                 * which breath_ctrl may already hold) only asks the operator to resend. */
                r.ok = settings_validate(base, &req, r.why, sizeof r.why) &&
                       settings_schedule(&cur, pending ? &next : NULL, now, &req.t0, r.why, sizeof r.why);
                if (r.ok) {
                    req.seq = base->seq + 1;
                    next = req;
                    pending = 1;
                    snprintf(buf, sizeof buf, "#%u %s %.1f/%.1f %.0f rpm 1:%.1f Vt %.0f desde t=%.3f", next.seq,
                             next.vcv ? "VCV" : "PCV", next.set.p_insp, next.set.peep, next.set.rr, next.set.ie,
                             next.vt * 1e3, next.t0 - t_start);
                    event(now, "AJUSTE_ACEPTADO", buf);
                } else {
                    event(now, "AJUSTE_RECHAZADO", r.why);
                }
                r.now = pending ? next : cur;
                MsgReply(rcvid, EOK, &r, sizeof r);
            }
            /* --- Copiloto. Solo el proceso que lanzamos (pid que pone el kernel) puede consultar y sugerir. --- */
            else if (m.hdr.type == BG_MSG_COPILOT_GET) {
                if (!use_copilot || info.pid != cps.pid || info.srcmsglen < (ssize_t)sizeof m.cget) {
                    cps.denied++;
                    MsgError(rcvid, EPERM);
                } else {
                    cp_scoid = info.scoid; /* así reconocemos su desconexión */
                    const cp_breath_t *last = cp_n ? &cp_ring[(cp_n - 1) % 4] : NULL;
                    /* una respiración que el copiloto (o el anterior, si se relanzó) aún no respondió */
                    if (last && last->breath > m.cget.after && (!cpc.n || last->breath > cpc.last_breath))
                        MsgReply(rcvid, EOK, last, sizeof *last);
                    else { /* respuesta diferida: se contesta al cerrar la próxima respiración (abajo) */
                        if (cp_rcvid != -1) MsgError(cp_rcvid, EBUSY);
                        cp_rcvid = rcvid;
                    }
                }
            }
            else if (m.hdr.type == BG_MSG_COPILOT_SUGGEST) {
                if (!use_copilot || info.pid != cps.pid || info.srcmsglen < (ssize_t)sizeof m.csug) {
                    cps.denied++;
                    snprintf(buf, sizeof buf, "SUGGEST de pid %d", info.pid);
                    event(now, "COPILOTO_NO_AUTORIZADO", buf);
                    MsgError(rcvid, EPERM);
                } else {
                    const cp_suggest_t *sg = &m.csug.s;
                    const cp_breath_t *mine = NULL; /* NUESTRA medición de esa respiración, no la copia del copiloto */
                    for (int i = 0; i < 4; i++)
                        if (cp_ring[i].breath && cp_ring[i].breath == sg->breath) mine = &cp_ring[i];
                    cps.last_cls = sg->cls;
                    cps.last_conf = sg->conf;
                    cps.last_breath = sg->breath;
                    cps.infer_ns_last = sg->infer_ns;
                    if (sg->infer_ns > cps.infer_ns_max) cps.infer_ns_max = sg->infer_ns;
                    int st0 = cpc.state;
                    char why[64];
                    int r = cp_check(&cpc, mine, sg, &cur, pending, now, why, sizeof why);
                    if (r == 1) {
                        snprintf(buf, sizeof buf, "#%u %s %.2f: %s", cpc.id, cp_class_name(cpc.cls), cpc.conf, cpc.text);
                        event(now, "COPILOTO_PROPONE", buf);
                    } else if (r == -1 && strncmp(why, cp_last_why, sizeof cp_last_why - 1)) { /* sin repetir */
                        snprintf(cp_last_why, sizeof cp_last_why, "%s", why);
                        event(now, "COPILOTO_BLOQUEADO", why);
                    }
                    if (st0 == CP_ST_PENDING && cpc.state == CP_ST_CLEARED) {
                        snprintf(buf, sizeof buf, "#%u: %d respiraciones normales", cpc.id, CP_PERSIST);
                        event(now, "COPILOTO_RESUELTO", buf);
                    }
                    if (r == -1) MsgError(rcvid, EINVAL);
                    else MsgReply(rcvid, EOK, NULL, 0);
                }
            }
            else if (m.hdr.type == BG_MSG_COPILOT_ACK) { /* el operador (HMI o ventd) responde una propuesta */
                bg_copilot_ack_reply_t ar = { 0 };
                if (info.srcmsglen < (ssize_t)sizeof m.cack) MsgError(rcvid, EINVAL);
                else if (use_copilot && cps.pid > 0 && (info.pid == cps.pid || m.cack.origin_pid == cps.pid)) {
                    /* el copiloto no puede confirmarse a sí mismo: ni directo ni a través de ventd */
                    cps.denied++;
                    snprintf(buf, sizeof buf, "pid %d%s propuesta %u", m.cack.origin_pid == cps.pid ?
                             m.cack.origin_pid : info.pid, m.cack.origin_pid == cps.pid ? " (via ventd)" : "",
                             m.cack.id);
                    event(now, "COPILOTO_AUTOCONFIRMA", buf);
                    MsgError(rcvid, EPERM);
                } else {
                    int has = 0;
                    bg_settings_t req;
                    /* no pisar en silencio un cambio del operador ya programado */
                    if (pending && m.cack.accept && m.cack.id == cpc.id && cpc.state == CP_ST_PENDING) {
                        ar.ok = 0;
                        snprintf(ar.why, sizeof ar.why, "busy: settings change #%u pending; answer again later",
                                 next.seq);
                    } else {
                        ar.ok = cp_ack(&cpc, m.cack.id, m.cack.accept, &cur, now, &has, &req, ar.why, sizeof ar.why);
                    }
                    if (ar.ok && has) { /* el mismo camino que un ajuste del operador: validar y programar */
                        char w2[64];
                        req.set.rise_s = cur.set.rise_s;
                        if (settings_validate(&cur, &req, w2, sizeof w2) &&
                            settings_schedule(&cur, NULL, now, &req.t0, w2, sizeof w2)) {
                            req.seq = cur.seq + 1;
                            next = req;
                            pending = 1;
                            snprintf(buf, sizeof buf, "#%u copiloto #%u: %s", next.seq, cpc.id, cpc.text);
                            event(now, "AJUSTE_ACEPTADO", buf);
                        } else {
                            cpc.state = CP_ST_PENDING; /* sigue pendiente: el operador puede reintentar */
                            cpc.n_accepted--;
                            ar.ok = 0;
                            snprintf(ar.why, sizeof ar.why, "%s", w2);
                        }
                    }
                    snprintf(buf, sizeof buf, "#%u %s: %s", m.cack.id, m.cack.accept ? "acepta" : "rechaza", ar.why);
                    event(now, !ar.ok ? "COPILOTO_RESP_INVALIDA" : m.cack.accept ? "COPILOTO_ACEPTADO"
                                                                                       : "COPILOTO_RECHAZADO", buf);
                    MsgReply(rcvid, EOK, &ar, sizeof ar);
                }
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
        if (m.pulse.code == _PULSE_CODE_UNBLOCK) { /* una señal al copiloto mientras esperaba su respiración */
            if (m.pulse.value.sival_int == cp_rcvid) { MsgError(cp_rcvid, EINTR); cp_rcvid = -1; }
            continue;
        }
        if (m.pulse.code == _PULSE_CODE_DISCONNECT) {
            if (m.pulse.scoid == ctrl_scoid) { ctrl_failed = 1; why = "CONTROL_MUERTO"; ctrl_scoid = -1; }
            if (m.pulse.scoid == cp_scoid) { /* el copiloto murió: la ventilación no se entera; se relanza */
                cp_scoid = -1;
                cp_rcvid = -1;
                if (now - t_start < secs - 1.0) {
                    cps.restarts++;
                    cps.pid = spawn_copilot(secs - (now - t_start));
                    snprintf(buf, sizeof buf, "pid %d", cps.pid);
                    event(now, "COPILOTO_RELANZADO", buf);
                }
            }
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
        last_s = s;
        if (ticks % 100 == 1) { /* cada 100 ms: signos vitales y registro de la planta (para la HMI y la tarea 4) */
            if (MsgSend(plant, &rv, sizeof rv, &last_vt, sizeof last_vt) == -1) break;
            if (last_vt.denied != denied_seen) {
                snprintf(buf, sizeof buf, "pid %d tipo 0x%x; %u rechazos", last_vt.denied_pid,
                         last_vt.denied_type, last_vt.denied);
                event(now, "COMANDO_NO_AUTORIZADO", buf);
                denied_seen = last_vt.denied;
                sec_alarm_t = now;
                al.active |= ALARM_SECURITY;
            }
            if (sec_alarm_t >= 0 && now - sec_alarm_t > SEC_ALARM_S) { al.active &= ~ALARM_SECURITY; sec_alarm_t = -1; }
        }

        if (pending && now >= next.t0) { /* empieza la respiración desde la que valen los ajustes nuevos */
            breath_base += settings_breath(&cur, next.t0 - 1e-6, NULL, NULL) + 1;
            cur = next;
            pending = 0;
            bf_dirty_next = 1; /* la primera respiración con ajustes nuevos es transitoria: no va al copiloto */
            init_vent(&backup, &cur, pbl_max);
            al.cfg = alarms_default_cfg(cur.set.peep); /* umbrales que dependen de la PEEP */
            snprintf(buf, sizeof buf, "#%u", cur.seq);
            event(now, "AJUSTE_APLICADO", buf);
        }
        double into;
        int insp;
        uint32_t breath = breath_base + settings_breath(&cur, now, &insp, &into);

        uint32_t up = alarms_update(&al, now, s.paw, s.flow);
        if (lung_update(&lm, now, s.paw, s.flow, insp, cur.set.peep)) { /* respiración cerrada: auto-PEEP */
            /* Con el tubo desconectado o en alivio de presión la espiración no es la de un pulmón R-C. */
            int ok = !(al.active & ALARM_DISCONNECT) && !relief;
            uint32_t ap = ok && lm.alarm ? ALARM_AUTO_PEEP : 0;
            up |= ap & ~al.active;
            al.active = (al.active & ~ALARM_AUTO_PEEP) | ap;
        }
        vtm_update(&vtm, now, s.flow, insp);
        if (vtm.breaths != vtm_breaths_seen) { /* respiración completa medida (tarea 2) */
            vtm_breaths_seen = vtm.breaths;
            double lo, hi;
            vt_limits(&cur, ibw, &lo, &hi);
            uint32_t vt_al = 0;
            if (vtm.vti < lo) vt_al |= ALARM_LOW_VT;
            if (vtm.vti > hi) vt_al |= ALARM_HIGH_VT;
            if (vtm.vti > 0.1 && vtm.vte < 0.8 * vtm.vti) vt_al |= ALARM_LEAK;
            uint32_t vt_bits = ALARM_LOW_VT | ALARM_HIGH_VT | ALARM_LEAK;
            up |= vt_al & ~al.active;
            al.active = (al.active & ~vt_bits) | vt_al;
        }
        /* Copiloto: features de la respiración, medidas aquí con los mismos sensores. Una respiración con alarmas de
         * circuito o de proceso, respaldo o alivio no es la de un pulmón R-C sano ni enfermo: no se le muestra. */
        if (backup_mode || relief || overriding ||
            (al.active & (ALARM_DISCONNECT | ALARM_APNEA | ALARM_HIGH_PRESSURE | ALARM_CTRL_DEAD | ALARM_CTRL_HANG)))
            bf_dirty = 1;
        if (bf_update(&bfm, now, s.paw, s.flow, insp, cur.set.peep, cur.vcv)) {
            cp_breath_t *r = &cp_ring[cp_n % 4];
            r->breath = ++cp_n;
            r->valid = bfm.valid && !bf_dirty;
            memcpy(r->f, bfm.f, sizeof r->f);
            r->set = bf_set;
            r->vti_ml = bfm.vti * 1e3;
            r->vte_ml = bfm.vte * 1e3;
            r->ibw_kg = ibw;
            r->auto_peep = lm.valid ? lm.auto_peep : -1;
            bf_dirty = bf_dirty_next;
            bf_dirty_next = 0;
            bf_set = cur;
            if (cp_rcvid != -1) { /* la consulta retenida del copiloto: MsgReply no bloquea a safety_mon */
                MsgReply(cp_rcvid, EOK, r, sizeof *r);
                cp_rcvid = -1;
            }
        }
        if (cp_expire(&cpc, now)) {
            snprintf(buf, sizeof buf, "#%u sin respuesta en %.0f s", cpc.id, CP_EXPIRE_S);
            event(now, "COPILOTO_VENCIDO", buf);
        }
        for (int b = 0; b < ALARM_COUNT; b++) {
            if (up & (1u << b)) {
                if ((1u << b) == ALARM_AUTO_PEEP)
                    snprintf(buf, sizeof buf, "auto-PEEP %.1f cmH2O, flujo final %.1f L/min, tau %.2f s",
                             lm.auto_peep, lm.q_ee * 60, lm.tau);
                else
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
        if (backup_mode && beats_since_spawn >= HANDBACK_BEATS && insp && into < 0.005) {
            backup_mode = 0;
            snprintf(buf, sizeof buf, "%u latidos del nuevo controlador", beats_since_spawn);
            event(now, "DEVUELVE_MANDO", buf);
        }

        actuators_t act;
        vent_step(&backup, now, now - last_tick, &s, &act); /* siempre corre: así su integral está listo */
        last_tick = now;
        if (relief) {
            act.blower = cur.set.peep / pbl_max;
            act.exh_open = 1.0;
            act.peep_cmd = cur.set.peep;
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
            ctrl_pid = spawn_ctrl(plant, &cur, secs - (t_ov - t_start));
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
    if (cps.pid > 0) kill(cps.pid, SIGTERM);
    name_detach(att, 0);
    rt_relax();
    size_t ev0 = n_events > N_EVENTS ? n_events - N_EVENTS : 0; /* los últimos N_EVENTS */
    for (size_t j = ev0; j < n_events; j++) {
        const event_t *e = &events[j % N_EVENTS];
        printf("%s: t=%.3f %s %s\n", PROG, e->t - t_start, e->what, e->detail);
    }
    printf("%s: %lu ticks, %u fallas del controlador, %zu eventos; vt_meter: %u respiraciones, Vti %.0f, "
           "Vte %.0f mL\n", PROG, ticks, failovers, n_events, vtm.breaths, vtm.vti * 1e3, vtm.vte * 1e3);
    printf("%s: peso ideal %.1f kg (talla %.1f cm); última respiración: auto-PEEP %.2f cmH2O, flujo final %.2f L/min, "
           "tau %.2f s, R %.1f, C %.1f mL/cmH2O\n", PROG, ibw, height_cm, lm.auto_peep, lm.q_ee * 60, lm.tau, lm.r_tot,
           lm.c * 1e3);
    if (use_copilot)
        printf("%s: copiloto: %u respiraciones medidas, %u sugerencias, %u bloqueadas, %u propuestas, %u aceptadas, "
               "%u rechazadas, %u vencidas, %u mensajes no autorizados, %u relanzamientos, árbol+reglas máx %.1f us\n",
               PROG, cp_n, cpc.n_suggest, cpc.n_blocked, cpc.n_proposed, cpc.n_accepted, cpc.n_rejected, cpc.n_expired,
               cps.denied, cps.restarts, cps.infer_ns_max / 1e3);
    if (out_path) {
        FILE *f = fopen(out_path, "w");
        if (!f) rt_die(PROG, out_path);
        fprintf(f, "t_s,evento,detalle\n");
        for (size_t j = ev0; j < n_events; j++) {
            const event_t *e = &events[j % N_EVENTS];
            fprintf(f, "%.6f,%s,%s\n", e->t, e->what, e->detail);
        }
        fclose(f);
    }
    return 0;
}
