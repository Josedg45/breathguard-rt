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
    BG_MSG_PATIENT,       /* carga: bg_patient_msg_t; respuesta: bg_window_t (diferida) */
};

/* Acople con Pulse (proceso patient). La planta corta su tiempo en ventanas de BG_WINDOW_S.
 * patient manda el resultado de la ventana k y pide la k+1; si aún no se cerró, la planta
 * retiene la respuesta hasta cerrarla. */
#define BG_WINDOW_S 0.020

typedef struct {
    uint32_t k;        /* ventana que hay que simular */
    uint32_t backlog;  /* ventanas ya cerradas después de k (cuánto va atrasado patient) */
    double t_end;      /* tiempo de planta al cerrar la ventana */
    double paw_mean;   /* presión promedio en la Y durante la ventana (cmH2O) */
    double last_dv;    /* corrección de volumen aplicada con el resultado anterior (L) */
    uint32_t lost;     /* ventanas que patient no alcanzó a simular */
} bg_window_t;

typedef struct {
    double v, q, r, c;           /* estado de Pulse: L, L/s, cmH2O/(L/s), L/cmH2O */
    double spo2, etco2, hr;      /* fracción 0-1, mmHg, lpm */
} bg_patient_state_t;

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

typedef struct {
    bg_hdr_t hdr;
    uint32_t have_result; /* 0 en el primer mensaje */
    uint32_t k;           /* ventana a la que corresponde el resultado */
    bg_patient_state_t st;
} bg_patient_msg_t;

#endif
