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
#include "settings.h"

#define BG_PNEUMATIC_NAME "breathguard_pneumatic"
#define BG_SAFETY_NAME "breathguard_safety"

/* Latido de breath_ctrl a safety_mon: pulso con el contador de secuencia en value. */
#define BG_PULSE_HEARTBEAT (_PULSE_CODE_MINAVAIL + 1)
/* Despedida: breath_ctrl termina a propósito; su desconexión no es una falla. */
#define BG_PULSE_BYE (_PULSE_CODE_MINAVAIL + 2)

enum {
    BG_MSG_READ = 0x4000, /* respuesta: sensors_t */
    BG_MSG_WRITE,         /* carga: actuators_t; respuesta vacía */
    BG_MSG_FAULT,         /* carga: plant_faults_t; respuesta vacía */
    BG_MSG_PATIENT,       /* carga: bg_patient_msg_t; respuesta: bg_window_t (diferida) */
    BG_MSG_OVERRIDE,      /* carga: bg_override_msg_t; respuesta vacía (solo safety_mon) */
    BG_MSG_VITALS,        /* respuesta: bg_vitals_t (a pneumatic_sim) */
    BG_MSG_SAFETY_STATUS, /* respuesta: bg_safety_status_t (a safety_mon) */
    BG_MSG_SETTINGS_SET,  /* carga: bg_settings_msg_t (hmi -> safety_mon); respuesta: bg_settings_reply_t */
    BG_MSG_SETTINGS_GET,  /* respuesta: bg_settings_t (breath_ctrl -> safety_mon) */
};

/* Acople con Pulse (proceso patient). La planta corta su tiempo en ventanas de BG_WINDOW_S.
 * patient manda el resultado de la ventana k y pide la k+1; si aún no se cerró, la planta
 * retiene la respuesta hasta cerrarla. */
#define BG_WINDOW_S 0.020

typedef struct {
    uint32_t k;        /* ventana que hay que simular */
    uint32_t backlog;  /* ventanas ya cerradas después de k (cuánto va atrasado patient) */
    double t_end;      /* tiempo de planta al cerrar la ventana */
    double paw_mean;   /* presión promedio en la boca del paciente durante la ventana (cmH2O) */
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

/* Signos vitales del paciente virtual, tal como los conoce la planta (último resultado de Pulse). */
typedef struct {
    int32_t have;        /* 0 si Pulse no está conectado */
    double spo2, etco2, hr;
    double age_s;        /* antigüedad del último resultado de Pulse */
    double r, c;         /* R (cmH2O/(L/s)) y C (L/cmH2O) actuales del pulmón */
    int32_t src;         /* quién manda los actuadores: 0 breath_ctrl, 1 safety_mon */
} bg_vitals_t;

/* Estado de safety_mon para la HMI. */
typedef struct {
    uint32_t alarms;       /* bits ALARM_* activos */
    int32_t backup;        /* 1 = ventilación de respaldo */
    uint32_t failovers;    /* fallas del controlador */
    uint32_t hmi_restarts; /* veces que se relanzó la HMI */
    uint32_t n_events;
    char last_event[48];
    bg_settings_t set;     /* ajustes mas recientes (vigentes o programados) */
    int32_t pending;       /* 1 = set aun no rige: empieza en set.t0 */
} bg_safety_status_t;

/* Pedido de cambio de ajustes del operador (la HMI lo reenvia). seq y t0 los pone safety_mon. */
typedef struct {
    bg_hdr_t hdr;
    bg_settings_t req;
} bg_settings_msg_t;

typedef struct {
    int32_t ok;            /* 1 aceptado */
    char why[64];          /* motivo del rechazo, u "ok" */
    bg_settings_t now;     /* ajustes resultantes */
} bg_settings_reply_t;

/* safety_mon toma (on = 1) o devuelve (on = 0) el mando de los actuadores. Mientras lo tiene, la
 * planta ignora los WRITE de breath_ctrl. */
typedef struct {
    bg_hdr_t hdr;
    int32_t on;
    actuators_t act;
} bg_override_msg_t;

typedef struct {
    bg_hdr_t hdr;
    uint32_t have_result; /* 0 en el primer mensaje */
    uint32_t k;           /* ventana a la que corresponde el resultado */
    bg_patient_state_t st;
} bg_patient_msg_t;

#endif
