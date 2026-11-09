/* evil_hmi.c - prueba de la tarea 4: una HMI comprometida que intenta mover los actuadores de la planta.
 *
 * Hace lo que podría hacer un atacante que toma control del proceso de la HMI (un bug explotable en la interfaz o
 * en su red), con los mismos privilegios:
 *   1. AUTH: intenta autorizarse a sí misma como controlador.
 *   2. OVERRIDE: intenta tomar el mando como si fuera safety_mon, con el soplador al máximo (60 cmH2O).
 *   3. WRITE: manda el soplador al máximo y la válvula espiratoria cerrada, cada 1 ms durante -d segundos.
 * Si la planta acepta el OVERRIDE, al final lo devuelve (on = 0) para no dejar la corrida bloqueada: un atacante
 * real no lo haría.
 *
 * Sale con 0 si el ataque se bloqueó (capa 1: la planta responde EPERM; capa 2: el kernel no deja conectarse) y con
 * 1 si la planta aceptó algo.
 * Uso: evil_hmi [-d segundos]
 */
#include <errno.h>
#include <getopt.h>
#include <string.h>
#include <sys/dispatch.h>
#include <unistd.h>

#include "bg_proto.h"
#include "rt_util.h"

#define PROG "evil_hmi"

static const char *res(int rc)
{
    return rc == -1 ? (errno == EPERM ? "RECHAZADO (EPERM)" : strerror(errno)) : "ACEPTADO";
}

int main(int argc, char **argv)
{
    double secs = 1.0;
    int opt;
    while ((opt = getopt(argc, argv, "d:")) != -1) {
        if (opt == 'd') secs = atof(optarg);
        else { fprintf(stderr, "uso: %s [-d segundos]\n", PROG); return 2; }
    }
    int coid = name_open(BG_PNEUMATIC_NAME, 0);
    if (coid == -1 && (errno == EACCES || errno == EPERM)) {
        /* Capa 2: con una política de seguridad, el tipo de la HMI no tiene channel_connect hacia la planta. El
         * kernel rechaza la conexión: el atacante no puede ni mandar el primer mensaje. */
        printf("%s: pid %d, t_abs=%.6f: name_open(planta) RECHAZADO por el kernel (%s): ataque bloqueado (capa 2)\n",
               PROG, getpid(), rt_now_s(), strerror(errno));
        return 0;
    }
    if (coid == -1) rt_die(PROG, "name_open(" BG_PNEUMATIC_NAME ")");
    actuators_t bad = { .blower = 1.0, .exh_open = 0.0, .peep_cmd = 5.0 };
    int accepted = 0;

    printf("%s: pid %d, t_abs=%.6f, ataque de %.1f s\n", PROG, getpid(), rt_now_s(), secs);
    bg_auth_msg_t au = { .hdr = { .type = BG_MSG_AUTH }, .pid = getpid() };
    int rc = MsgSend(coid, &au, sizeof au, NULL, 0);
    printf("%s: 1. AUTH para sí misma: %s\n", PROG, res(rc));
    accepted += rc != -1;

    bg_override_msg_t ov = { .hdr = { .type = BG_MSG_OVERRIDE }, .on = 1, .act = bad };
    rc = MsgSend(coid, &ov, sizeof ov, NULL, 0);
    int ov_taken = rc != -1;
    printf("%s: 2. OVERRIDE con el soplador al máximo: %s\n", PROG, res(rc));
    accepted += ov_taken;

    bg_write_msg_t wr = { .hdr = { .type = BG_MSG_WRITE }, .act = bad };
    unsigned long ok = 0, eperm = 0, other = 0;
    double t0 = rt_now_s();
    while (rt_now_s() - t0 < secs) {
        if (MsgSend(coid, &wr, sizeof wr, NULL, 0) != -1) ok++;
        else if (errno == EPERM) eperm++;
        else other++;
        usleep(1000);
    }
    printf("%s: 3. WRITE con el soplador al máximo: %lu aceptados, %lu rechazados (EPERM), %lu otros errores\n",
           PROG, ok, eperm, other);
    accepted += ok > 0;

    if (ov_taken) {
        ov.on = 0;
        MsgSend(coid, &ov, sizeof ov, NULL, 0);
        printf("%s: devuelvo el OVERRIDE (on = 0) para no dejar la corrida bloqueada\n", PROG);
    }
    printf("%s: t_abs=%.6f %s\n", PROG, rt_now_s(),
           accepted ? "ATAQUE ACEPTADO: la planta obedeció a la HMI" : "ataque bloqueado: la planta rechazó todo");
    return accepted ? 1 : 0;
}
