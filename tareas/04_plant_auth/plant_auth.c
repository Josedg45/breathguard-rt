/* plant_auth.c - Tarea 4 (B capa 1): autorización por pid en pneumatic_sim. Ver README.md.
 *
 * pneumatic_sim llama a auth_check() con cada mensaje que recibe (salvo los _IO_CONNECT de name_open). Si
 * devuelve 0, la planta responde EPERM al emisor, no toca los actuadores y registra el intento.
 */
#include <string.h>

#include "plant_auth.h"

void auth_init(plant_auth_t *a)
{
    memset(a, 0, sizeof *a);
}

int auth_check(plant_auth_t *a, const struct _msg_info *info, uint16_t type, const bg_auth_msg_t *auth)
{
    (void)a;
    (void)info;
    (void)type;
    (void)auth;
    /* TODO 1: los mensajes que no mueven actuadores (READ, VITALS, PATIENT, FAULT y cualquier tipo desconocido)
     *         se aceptan siempre. Usa un switch sobre type. */
    /* TODO 2: BG_MSG_AUTH. En este orden:
     *         a) si el emisor mandó menos bytes que sizeof(bg_auth_msg_t), rechazar (mira info->srcmsglen);
     *         b) si auth->pid <= 0, rechazar;
     *         c) si nadie es dueño todavía (safety_pid == 0), el emisor pasa a serlo;
     *         d) si el emisor no es el dueño, rechazar;
     *         e) ctrl_pid = auth->pid y aceptar. */
    /* TODO 3: BG_MSG_WRITE: aceptar solo si hay un controlador autorizado y el emisor es ese proceso. */
    /* TODO 4: BG_MSG_OVERRIDE: aceptar solo si hay dueño y el emisor es el dueño. */
    return 1; /* esqueleto: acepta todo, como antes de esta tarea */
}
