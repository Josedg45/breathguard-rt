/* ventd.c - el ventilador como archivos: resource manager de QNX en /dev/ventilator. Prioridad BAJA (10).
 *
 * En QNX un "driver" es un proceso cualquiera que se apropia de un trozo del espacio de nombres (resmgr_attach).
 * Cuando alguien hace open/read/write sobre esa ruta, la libc lo convierte en mensajes que llegan a este proceso.
 * Así cualquier herramienta (cat, echo, un script, otro lenguaje) usa el ventilador sin conocer nuestro protocolo:
 *
 *   cat /dev/ventilator/status       estado: modo y ajustes, Paw, flujo, signos vitales, alarmas, energía, lazo
 *   cat /dev/ventilator/alarms       alarmas activas, una por línea ("none" si no hay)
 *   cat /dev/ventilator/settings     ajustes vigentes (en el formato que acepta la escritura) y el último resultado
 *   echo "p_insp=22 peep=6" > /dev/ventilator/settings
 *                                    pedido de cambio. ventd NO valida ni aplica: lo reenvía a safety_mon, igual que
 *                                    la HMI. Si safety_mon lo rechaza, write() falla con EINVAL (EAGAIN si es "busy").
 *
 * ventd no tiene conexión con la planta: lee todo de safety_mon (BG_MSG_MONITOR, BG_MSG_SAFETY_STATUS) y de
 * power_mgr. Si ventd muere o se cuelga, la ventilación no se entera.
 *
 * Uso: ventd [-p prio] [-s segundos]   (0 = sin límite)
 */
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <string.h>
/* iofunc.h antes que dispatch.h: así RESMGR_OCB_T es iofunc_ocb_t y no void. */
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/stat.h>
#include <unistd.h>

#include "alarms.h"
#include "bg_power.h"
#include "bg_proto.h"
#include "rt_util.h"
#include "settings.h"

#define PROG "ventd"
#define DIR "/dev/ventilator"

static iofunc_attr_t a_status, a_alarms, a_settings;
static int safety = -1, power = -1;
static char last_result[160] = "none";

/* Copia del contenido que se está leyendo: se genera al empezar una lectura (offset 0), así un cat que lee en
 * varios trozos ve un solo instante. Un hilo: basta con una. */
static char snap[2048];
static size_t snap_len;
static const void *snap_ocb;

static int ask(int *coid, const char *name, const void *q, size_t qn, void *r, size_t rn)
{
    if (*coid == -1) *coid = name_open(name, 0);
    if (*coid == -1) return -1;
    if (MsgSend(*coid, q, qn, r, rn) == -1) {
        name_close(*coid);
        *coid = -1;
        return -1;
    }
    return 0;
}

static int get_safety(bg_safety_status_t *ss, bg_monitor_t *mo)
{
    bg_hdr_t rs = { .type = BG_MSG_SAFETY_STATUS }, rm = { .type = BG_MSG_MONITOR };
    if (ask(&safety, BG_SAFETY_NAME, &rs, sizeof rs, ss, sizeof *ss) == -1) return -1;
    if (mo && ask(&safety, BG_SAFETY_NAME, &rm, sizeof rm, mo, sizeof *mo) == -1) return -1;
    return 0;
}

static size_t put_settings(char *b, size_t n, const bg_settings_t *s)
{
    return (size_t)snprintf(b, n, "mode=%s p_insp=%.1f peep=%.1f rr=%.1f ie=%.1f vt=%.0f\n", s->vcv ? "vcv" : "pcv",
                            s->set.p_insp, s->set.peep, s->set.rr, s->set.ie, s->vt * 1e3);
}

static size_t put_alarms(char *b, size_t n, uint32_t al)
{
    size_t k = 0;
    for (int i = 0; i < ALARM_COUNT && k < n; i++)
        if (al & (1u << i)) k += (size_t)snprintf(b + k, n - k, "%s\n", alarm_name(1u << i));
    if (!k) k = (size_t)snprintf(b, n, "none\n");
    return k < n ? k : n - 1;
}

static void build(const iofunc_attr_t *attr)
{
    bg_safety_status_t ss;
    bg_monitor_t mo;
    char *b = snap;
    size_t n = sizeof snap, k = 0;
#define PUT(...) k += (k < n ? (size_t)snprintf(b + k, n - k, __VA_ARGS__) : 0)
    if (get_safety(&ss, attr == &a_status ? &mo : NULL) == -1) {
        snap_len = (size_t)snprintf(b, n, "error: safety_mon no disponible\n");
        return;
    }
    if (attr == &a_alarms) {
        snap_len = put_alarms(b, n, ss.alarms);
        return;
    }
    if (attr == &a_settings) {
        k = put_settings(b, n, &ss.set);
        PUT("seq=%u pending=%d\n", ss.set.seq, ss.pending);
        PUT("last=%s\n", last_result);
        snap_len = k < n ? k : n - 1;
        return;
    }
    const bg_vitals_t *v = &mo.vt;
    const bg_loop_stats_t *L = &ss.loop;
    PUT("clock_s=%.3f\n", rt_now_s()); /* reloj del sistema (ClockCycles): el mismo de los registros */
    PUT("source=%s\n", ss.backup ? "backup (safety_mon)" : "breath_ctrl");
    k += put_settings(b + k, n - k, &ss.set);
    PUT("pending=%d seq=%u\n", ss.pending, ss.set.seq);
    PUT("paw_cmh2o=%.2f flow_lpm=%.1f\n", mo.s.paw, mo.s.flow * 60);
    if (v->have) PUT("spo2_pct=%.1f etco2_mmhg=%.1f hr_bpm=%.0f fio2=%.2f\n", v->spo2 * 100, v->etco2, v->hr, v->fio2);
    else PUT("spo2_pct=na etco2_mmhg=na hr_bpm=na (sin Pulse)\n");
    PUT("ibw_kg=%.1f auto_peep_cmh2o=%.2f\n", ss.ibw_kg, ss.auto_peep);
    PUT("alarms=0x%x failovers=%u hmi_restarts=%u denied=%u\n", ss.alarms, ss.failovers, ss.hmi_restarts, ss.denied);
    PUT("loop_p99_ms=%.3f loop_max_ms=%.3f loop_late=%u loop_cycles=%u\n", L->p99_ms, L->max_ms, L->late, L->cycles);
    bg_hdr_t rp = { .type = BG_MSG_POWER_STATUS };
    bg_power_status_t ps;
    if (ask(&power, BG_POWER_NAME, &rp, sizeof rp, &ps, sizeof ps) == 0)
        PUT("power=%s soc_pct=%.0f autonomy_min=%.0f\n", ps.bits & PWR_ON_BATTERY ? "battery" : "mains", ps.soc * 100,
            ps.autonomy_s / 60);
    PUT("last_event=%s\n", ss.last_event[0] ? ss.last_event : "-");
#undef PUT
    snap_len = k < n ? k : n - 1;
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    int st = iofunc_read_verify(ctp, msg, ocb, NULL); /* ¿abierto para lectura? */
    if (st != EOK) return st;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
    if (ocb->offset == 0 || snap_ocb != ocb) {
        build(ocb->attr);
        snap_ocb = ocb;
    }
    size_t off = (size_t)ocb->offset < snap_len ? (size_t)ocb->offset : snap_len;
    size_t nb = snap_len - off < msg->i.nbytes ? snap_len - off : msg->i.nbytes;
    ocb->offset += nb;
    if (nb) ocb->attr->flags |= IOFUNC_ATTR_ATIME;
    _IO_SET_READ_NBYTES(ctp, nb);      /* lo que devolverá read() */
    SETIOV(ctp->iov, snap + off, nb);  /* los datos van en la respuesta, sin copia extra */
    return _RESMGR_NPARTS(nb ? 1 : 0);
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb)
{
    int st = iofunc_write_verify(ctp, msg, ocb, NULL); /* ¿abierto para escritura? */
    if (st != EOK) return st;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
    if (ocb->attr != &a_settings) return EROFS; /* root puede abrir para escribir un 0444: lo frenamos aquí */
    char cmd[256];
    size_t nb = msg->i.nbytes;
    if (nb >= sizeof cmd) return E2BIG;
    /* Los datos vienen detrás de la cabecera del mensaje; resmgr_msgget los copia (o los lee del cliente). */
    if (resmgr_msgget(ctp, cmd, nb, sizeof msg->i) != (ssize_t)nb) return EIO;
    cmd[nb] = 0;
    char *nl = strchr(cmd, '\n');
    char shown[64];
    snprintf(shown, sizeof shown, "%.*s", nl ? (int)(nl - cmd) : (int)nb, cmd);

    bg_safety_status_t ss;
    if (get_safety(&ss, NULL) == -1) return EAGAIN;
    bg_settings_msg_t sm = { .hdr = { .type = BG_MSG_SETTINGS_SET } };
    bg_settings_reply_t rep;
    char err[64];
    int ret = EOK;
    if (settings_parse_cmd(cmd, &ss.set, &sm.req, err, sizeof err) == -1) {
        snprintf(last_result, sizeof last_result, "NAK %s (%s)", err, shown);
        ret = EINVAL;
    } else if (ask(&safety, BG_SAFETY_NAME, &sm, sizeof sm, &rep, sizeof rep) == -1) {
        snprintf(last_result, sizeof last_result, "NAK safety_mon no disponible (%s)", shown);
        ret = EAGAIN;
    } else if (!rep.ok) {
        snprintf(last_result, sizeof last_result, "NAK %s (%s)", rep.why, shown);
        ret = strncmp(rep.why, SET_BUSY_PREFIX, strlen(SET_BUSY_PREFIX)) ? EINVAL : EAGAIN;
    } else {
        snprintf(last_result, sizeof last_result, "ACK #%u desde t=%.3f (%s)", rep.now.seq, rep.now.t0, shown);
    }
    printf("%s: escritura de pid %d: %s\n", PROG, ctp->info.pid, last_result);
    fflush(stdout);
    if (ret != EOK) return ret;
    _IO_SET_WRITE_NBYTES(ctp, nb);
    ocb->attr->flags |= IOFUNC_ATTR_MTIME | IOFUNC_ATTR_CTIME;
    return EOK;
}

int main(int argc, char **argv)
{
    int prio = 10, opt;
    double secs = 0;
    while ((opt = getopt(argc, argv, "p:s:")) != -1) {
        switch (opt) {
        case 'p': prio = atoi(optarg); break;
        case 's': secs = atof(optarg); break;
        default: fprintf(stderr, "uso: %s [-p prio] [-s segundos]\n", PROG); return 1;
        }
    }
    /* dispatch: el bucle genérico de recepción de mensajes de QNX (crea el canal y despacha a los manejadores). */
    dispatch_t *dpp = dispatch_create();
    if (!dpp) rt_die(PROG, "dispatch_create");

    /* Tablas de manejadores: iofunc_func_init pone los de por defecto (open, close, stat, chmod...); solo
     * cambiamos read y write. */
    resmgr_connect_funcs_t cfuncs;
    resmgr_io_funcs_t iofuncs;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cfuncs, _RESMGR_IO_NFUNCS, &iofuncs);
    iofuncs.read = io_read;
    iofuncs.write = io_write;

    resmgr_attr_t rattr = { .nparts_max = 1, .msg_max_size = 2048 };
    struct {
        iofunc_attr_t *a;
        const char *path;
        mode_t mode;
    } files[] = {
        { &a_status, DIR "/status", S_IFREG | 0444 },
        { &a_alarms, DIR "/alarms", S_IFREG | 0444 },
        { &a_settings, DIR "/settings", S_IFREG | 0664 },
    };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        /* attr: los "metadatos" del archivo (modo, dueño, tiempos), como un inodo. */
        iofunc_attr_init(files[i].a, files[i].mode, NULL, NULL);
        /* resmgr_attach: desde ahora, open() de esta ruta llega a nuestro canal. */
        if (resmgr_attach(dpp, &rattr, files[i].path, _FTYPE_ANY, 0, &cfuncs, &iofuncs, files[i].a) == -1)
            rt_die(PROG, files[i].path);
    }
    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    if (!ctp) rt_die(PROG, "dispatch_context_alloc");
    rt_setup(PROG, prio);
    printf("%s: listo en %s/{status,alarms,settings}, prio %d\n", PROG, DIR, prio);
    fflush(stdout);

    double t0 = rt_now_s();
    for (;;) {
        if (secs > 0) { /* para las pruebas: no quedarse para siempre */
            double left = secs - (rt_now_s() - t0);
            if (left <= 0) break;
            uint64_t ns = (uint64_t)(left * 1e9);
            TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &ns, NULL);
        }
        if (dispatch_block(ctp) == NULL) { /* espera un mensaje (bloqueado, sin gastar CPU) */
            if (errno == ETIMEDOUT || errno == EINTR) continue;
            rt_die(PROG, "dispatch_block");
        }
        dispatch_handler(ctp); /* lo entrega al manejador que toque (io_read, io_write, open...) */
    }
    rt_relax();
    printf("%s: fin\n", PROG);
    return 0;
}
