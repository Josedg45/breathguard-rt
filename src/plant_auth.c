/* plant_auth.c - actuator authorization by sender pid in pneumatic_sim (security layer 1).
 *
 * pneumatic_sim calls auth_check() for every message it receives (except name_open's _IO_CONNECT). If it
 * returns 0, the plant replies EPERM to the sender, leaves the actuators alone and logs the attempt.
 */
#include <string.h>

#include "plant_auth.h"

void auth_init(plant_auth_t *a)
{
    memset(a, 0, sizeof *a);
}

int auth_check(plant_auth_t *a, const struct _msg_info *info, uint16_t type, const bg_auth_msg_t *auth)
{
    /* info->pid is written by the kernel: the only "who is sending" data that can be trusted. */
    pid_t sender = info->pid;

    switch (type) {
    case BG_MSG_AUTH:
        /* a) Truncated message: the union's pid field would hold leftovers of the previous message. srcmsglen
         *    is signed (_Ssize64t), hence the cast of sizeof. */
        if (auth == NULL || info->srcmsglen < (_Ssize64t)sizeof(bg_auth_msg_t))
            return 0;
        /* b) A pid <= 0 is not a process: refuse it BEFORE naming an owner, so that an invalid AUTH does not
         *    hand ownership to anyone. */
        if (auth->pid <= 0)
            return 0;
        /* c) Trust on first use: the first valid AUTH names the owner (safety_mon starts first). */
        if (a->safety_pid == 0)
            a->safety_pid = sender;
        /* d) Only the owner may change who the controller is. */
        if (sender != a->safety_pid)
            return 0;
        /* e) The new controller replaces the previous one: an old breath_ctrl loses its permission. */
        a->ctrl_pid = auth->pid;
        return 1;

    case BG_MSG_WRITE:
        return a->ctrl_pid > 0 && sender == a->ctrl_pid;

    case BG_MSG_OVERRIDE:
        return a->safety_pid > 0 && sender == a->safety_pid;

    default:
        /* READ, VITALS, PATIENT, FAULT and unknown types do not move actuators (the plant answers ENOSYS to
         * what it does not know). */
        return 1;
    }
}
