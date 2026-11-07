/* bg_proto.h - mensajes QNX entre procesos de BreathGuard RT.
 *
 * pneumatic_sim se registra con name_attach() bajo BG_PNEUMATIC_NAME. Los clientes lo encuentran
 * con name_open() y le hablan con MsgSend(). Cada mensaje empieza con un tipo de 16 bits, igual que
 * los mensajes del sistema; los nuestros usan valores por encima de _IO_MAX para no confundirse
 * con ellos. */
#ifndef BG_PROTO_H
#define BG_PROTO_H

#include <stdint.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>

#include "bg_types.h"

#define BG_PNEUMATIC_NAME "breathguard_pneumatic"

enum {
    BG_MSG_READ = 0x4000, /* respuesta: sensors_t */
    BG_MSG_WRITE,         /* carga: actuators_t; respuesta vacía */
    BG_MSG_FAULT,         /* carga: plant_faults_t; respuesta vacía */
};

typedef struct {
    uint16_t type;
    uint16_t pad;
} bg_hdr_t;

typedef struct {
    bg_hdr_t hdr;
    actuators_t act;
} bg_write_msg_t;

typedef struct {
    bg_hdr_t hdr;
    plant_faults_t faults;
} bg_fault_msg_t;

#endif
