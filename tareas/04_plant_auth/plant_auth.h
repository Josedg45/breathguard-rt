/* plant_auth.h - Tarea 4: quién puede mover los actuadores de pneumatic_sim. Ver README.md.
 * No modifiques este archivo: pneumatic_sim y la prueba dependen de él. */
#ifndef PLANT_AUTH_H
#define PLANT_AUTH_H

#include <sys/neutrino.h>
#include <sys/types.h>

#include "bg_proto.h"

typedef struct {
    pid_t safety_pid; /* dueño de la autorización (el primero que mandó un AUTH válido); 0 = nadie todavía */
    pid_t ctrl_pid;   /* único proceso que puede mandar WRITE; 0 = ninguno */
} plant_auth_t;

void auth_init(plant_auth_t *a);

/* Decide si pneumatic_sim atiende un mensaje.
 *   info: lo que el kernel llenó en MsgReceive() (quién envía, cuántos bytes mandó...).
 *   type: el tipo del mensaje (m.hdr.type).
 *   auth: el mensaje si type == BG_MSG_AUTH; si no, NULL.
 * Devuelve 1 para atenderlo y 0 para rechazarlo. Con BG_MSG_AUTH aceptado, además actualiza *a. */
int auth_check(plant_auth_t *a, const struct _msg_info *info, uint16_t type, const bg_auth_msg_t *auth);

#endif
