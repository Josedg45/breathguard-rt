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
    /* info->pid lo escribe el kernel: es el único dato de "quién envía" en el que se puede confiar. */
    pid_t sender = info->pid;

    switch (type) {
    case BG_MSG_AUTH:
        /* a) Mensaje truncado: el campo pid del union tendría basura del mensaje anterior. srcmsglen es con
         *    signo (_Ssize64t), por eso se convierte el sizeof: comparar con signo distinto da avisos. */
        if (auth == NULL || info->srcmsglen < (_Ssize64t)sizeof(bg_auth_msg_t))
            return 0;
        /* b) Un pid <= 0 no es un proceso: rechazarlo ANTES de nombrar dueño, para que un AUTH inválido
         *    no le regale la propiedad a nadie. */
        if (auth->pid <= 0)
            return 0;
        /* c) Trust on first use: el primero con un AUTH válido es el dueño (safety_mon arranca primero). */
        if (a->safety_pid == 0)
            a->safety_pid = sender;
        /* d) Solo el dueño puede cambiar quién es el controlador. */
        if (sender != a->safety_pid)
            return 0;
        /* e) El nuevo controlador reemplaza al anterior: un breath_ctrl viejo queda sin permiso. */
        a->ctrl_pid = auth->pid;
        return 1;

    case BG_MSG_WRITE:
        return a->ctrl_pid > 0 && sender == a->ctrl_pid;

    case BG_MSG_OVERRIDE:
        return a->safety_pid > 0 && sender == a->safety_pid;

    default:
        /* READ, VITALS, PATIENT, FAULT y tipos desconocidos no mueven actuadores (la planta contesta ENOSYS
         * a lo que no conoce). */
        return 1;
    }
}
