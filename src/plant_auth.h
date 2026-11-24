/* plant_auth.h - who may move pneumatic_sim's actuators. The decision uses only what the QNX kernel fills
 * in on MsgReceive() (struct _msg_info): the sender's pid cannot be forged by the sender. */
#ifndef PLANT_AUTH_H
#define PLANT_AUTH_H

#include <sys/neutrino.h>
#include <sys/types.h>

#include "bg_proto.h"

typedef struct {
    pid_t safety_pid; /* owner of the authorization (first sender of a valid AUTH); 0 = nobody yet */
    pid_t ctrl_pid;   /* only process allowed to send WRITE; 0 = none */
} plant_auth_t;

void auth_init(plant_auth_t *a);

/* Decides whether pneumatic_sim serves a message.
 *   info: what the kernel filled in on MsgReceive() (sender, bytes sent...).
 *   type: the message type (m.hdr.type).
 *   auth: the message if type == BG_MSG_AUTH; NULL otherwise.
 * Returns 1 to serve it and 0 to refuse it. An accepted BG_MSG_AUTH also updates *a. */
int auth_check(plant_auth_t *a, const struct _msg_info *info, uint16_t type, const bg_auth_msg_t *auth);

#endif
