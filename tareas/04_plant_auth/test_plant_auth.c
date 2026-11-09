/* test_plant_auth.c - prueba de la tarea 4. No la modifiques.
 * Corre en QNX (necesita struct _msg_info). Fabrica lo que el kernel llenaría en MsgReceive() para cada caso. */
#include <stdio.h>
#include <string.h>

#include "plant_auth.h"

#define SAFETY 1001
#define CTRL 2002
#define CTRL2 2003
#define HMI 3003

static int fails;

static int send(plant_auth_t *a, pid_t from, uint16_t type, int32_t auth_pid, ssize_t len)
{
    struct _msg_info info;
    memset(&info, 0, sizeof info);
    info.pid = from;
    info.srcmsglen = info.msglen = len;
    bg_auth_msg_t m = { .hdr = { .type = type }, .pid = auth_pid };
    return auth_check(a, &info, type, type == BG_MSG_AUTH ? &m : NULL);
}

static void check(const char *name, int got, int want)
{
    printf("%-58s %s  (espera %s)\n", name, got == want ? "OK  " : "FALLA", want ? "aceptar" : "rechazar");
    if (got != want) fails++;
}

static void state(const char *name, const plant_auth_t *a, pid_t safety, pid_t ctrl)
{
    int ok = a->safety_pid == safety && a->ctrl_pid == ctrl;
    printf("%-58s %s  (espera dueño %d, control %d; hay %d, %d)\n", name, ok ? "OK  " : "FALLA", safety, ctrl,
           a->safety_pid, a->ctrl_pid);
    if (!ok) fails++;
}

int main(void)
{
    const ssize_t AUTH_LEN = sizeof(bg_auth_msg_t), WRITE_LEN = sizeof(bg_write_msg_t),
                  OV_LEN = sizeof(bg_override_msg_t), HDR_LEN = sizeof(bg_hdr_t);
    plant_auth_t a;
    auth_init(&a);

    check("READ de cualquiera", send(&a, HMI, BG_MSG_READ, 0, HDR_LEN), 1);
    check("WRITE antes de autorizar a nadie", send(&a, CTRL, BG_MSG_WRITE, 0, WRITE_LEN), 0);
    check("OVERRIDE sin dueño", send(&a, SAFETY, BG_MSG_OVERRIDE, 0, OV_LEN), 0);
    check("AUTH truncado (solo la cabecera, 4 bytes)", send(&a, SAFETY, BG_MSG_AUTH, CTRL, HDR_LEN), 0);
    check("AUTH con pid 0", send(&a, SAFETY, BG_MSG_AUTH, 0, AUTH_LEN), 0);
    state("  ...los AUTH inválidos no reclaman la planta", &a, 0, 0);
    check("AUTH de safety_mon: autoriza a breath_ctrl", send(&a, SAFETY, BG_MSG_AUTH, CTRL, AUTH_LEN), 1);
    state("  ...safety_mon es el dueño; breath_ctrl, el controlador", &a, SAFETY, CTRL);
    check("WRITE de breath_ctrl", send(&a, CTRL, BG_MSG_WRITE, 0, WRITE_LEN), 1);
    check("WRITE de la HMI (ataque)", send(&a, HMI, BG_MSG_WRITE, 0, WRITE_LEN), 0);
    check("OVERRIDE de la HMI (ataque)", send(&a, HMI, BG_MSG_OVERRIDE, 0, OV_LEN), 0);
    check("AUTH de la HMI para sí misma (ataque)", send(&a, HMI, BG_MSG_AUTH, HMI, AUTH_LEN), 0);
    state("  ...la HMI no cambió nada", &a, SAFETY, CTRL);
    check("OVERRIDE de safety_mon", send(&a, SAFETY, BG_MSG_OVERRIDE, 0, OV_LEN), 1);
    check("WRITE de safety_mon (no es el controlador)", send(&a, SAFETY, BG_MSG_WRITE, 0, WRITE_LEN), 0);
    check("relanzamiento: AUTH de safety_mon para el nuevo", send(&a, SAFETY, BG_MSG_AUTH, CTRL2, AUTH_LEN), 1);
    check("WRITE del controlador viejo", send(&a, CTRL, BG_MSG_WRITE, 0, WRITE_LEN), 0);
    check("WRITE del controlador nuevo", send(&a, CTRL2, BG_MSG_WRITE, 0, WRITE_LEN), 1);
    check("FAULT de cualquiera (banco de pruebas)", send(&a, HMI, BG_MSG_FAULT, 0, HDR_LEN), 1);
    check("VITALS de cualquiera", send(&a, HMI, BG_MSG_VITALS, 0, HDR_LEN), 1);
    check("PATIENT (Pulse)", send(&a, 4004, BG_MSG_PATIENT, 0, HDR_LEN), 1);
    check("tipo desconocido (la planta responde ENOSYS)", send(&a, HMI, 0x7777, 0, HDR_LEN), 1);
    printf(fails ? "\n%d pruebas fallan\n" : "\nTodo bien\n", fails);
    return fails != 0;
}
