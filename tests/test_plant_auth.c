/* test_plant_auth.c - unit test of src/plant_auth.c. Runs on QNX (needs struct _msg_info): it builds what
 * the kernel would fill in on MsgReceive() for each case. */
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
    printf("%-58s %s  (expects %s)\n", name, got == want ? "OK  " : "FAIL", want ? "accept" : "reject");
    if (got != want) fails++;
}

static void state(const char *name, const plant_auth_t *a, pid_t safety, pid_t ctrl)
{
    int ok = a->safety_pid == safety && a->ctrl_pid == ctrl;
    printf("%-58s %s  (expects owner %d, controller %d; has %d, %d)\n", name, ok ? "OK  " : "FAIL", safety, ctrl,
           a->safety_pid, a->ctrl_pid);
    if (!ok) fails++;
}

int main(void)
{
    const ssize_t AUTH_LEN = sizeof(bg_auth_msg_t), WRITE_LEN = sizeof(bg_write_msg_t),
                  OV_LEN = sizeof(bg_override_msg_t), HDR_LEN = sizeof(bg_hdr_t);
    plant_auth_t a;
    auth_init(&a);

    check("READ from anyone", send(&a, HMI, BG_MSG_READ, 0, HDR_LEN), 1);
    check("WRITE before anyone is authorized", send(&a, CTRL, BG_MSG_WRITE, 0, WRITE_LEN), 0);
    check("OVERRIDE without an owner", send(&a, SAFETY, BG_MSG_OVERRIDE, 0, OV_LEN), 0);
    check("truncated AUTH (header only, 4 bytes)", send(&a, SAFETY, BG_MSG_AUTH, CTRL, HDR_LEN), 0);
    check("AUTH with pid 0", send(&a, SAFETY, BG_MSG_AUTH, 0, AUTH_LEN), 0);
    state("  ...invalid AUTHs do not claim the plant", &a, 0, 0);
    check("AUTH from safety_mon: authorizes breath_ctrl", send(&a, SAFETY, BG_MSG_AUTH, CTRL, AUTH_LEN), 1);
    state("  ...safety_mon owns it; breath_ctrl controls", &a, SAFETY, CTRL);
    check("WRITE from breath_ctrl", send(&a, CTRL, BG_MSG_WRITE, 0, WRITE_LEN), 1);
    check("WRITE from the HMI (attack)", send(&a, HMI, BG_MSG_WRITE, 0, WRITE_LEN), 0);
    check("OVERRIDE from the HMI (attack)", send(&a, HMI, BG_MSG_OVERRIDE, 0, OV_LEN), 0);
    check("AUTH from the HMI for itself (attack)", send(&a, HMI, BG_MSG_AUTH, HMI, AUTH_LEN), 0);
    state("  ...the HMI changed nothing", &a, SAFETY, CTRL);
    check("OVERRIDE from safety_mon", send(&a, SAFETY, BG_MSG_OVERRIDE, 0, OV_LEN), 1);
    check("WRITE from safety_mon (not the controller)", send(&a, SAFETY, BG_MSG_WRITE, 0, WRITE_LEN), 0);
    check("restart: AUTH from safety_mon for the new one", send(&a, SAFETY, BG_MSG_AUTH, CTRL2, AUTH_LEN), 1);
    check("WRITE from the old controller", send(&a, CTRL, BG_MSG_WRITE, 0, WRITE_LEN), 0);
    check("WRITE from the new controller", send(&a, CTRL2, BG_MSG_WRITE, 0, WRITE_LEN), 1);
    check("FAULT from anyone (test bench)", send(&a, HMI, BG_MSG_FAULT, 0, HDR_LEN), 1);
    check("VITALS from anyone", send(&a, HMI, BG_MSG_VITALS, 0, HDR_LEN), 1);
    check("PATIENT (Pulse)", send(&a, 4004, BG_MSG_PATIENT, 0, HDR_LEN), 1);
    check("unknown type (the plant answers ENOSYS)", send(&a, HMI, 0x7777, 0, HDR_LEN), 1);
    printf(fails ? "\nRESULT: %d check(s) failed\n" : "\nRESULT: all checks passed\n", fails);
    return fails != 0;
}
