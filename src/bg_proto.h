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

typedef struct {
    uint16_t type;
    uint16_t pad;
} bg_hdr_t;

enum {
    BG_MSG_READ = 0x4000, /* respuesta: sensors_t */
    BG_MSG_WRITE,         /* carga: actuators_t; respuesta vacía */
    BG_MSG_FAULT,         /* carga: plant_faults_t; respuesta vacía */
    BG_MSG_PATIENT,       /* carga: bg_patient_msg_t; respuesta: bg_window_t (diferida) */
    BG_MSG_OVERRIDE,      /* carga: bg_override_msg_t; respuesta vacía (solo safety_mon) */
    BG_MSG_VITALS,        /* respuesta: bg_vitals_t (a pneumatic_sim) */
    BG_MSG_SAFETY_STATUS, /* respuesta: bg_safety_status_t (a safety_mon) */
    BG_MSG_SETTINGS_SET,  /* carga: bg_settings_msg_t (hmi -> safety_mon); respuesta: bg_settings_reply_t */
    BG_MSG_SETTINGS_GET,  /* carga: bg_settings_get_t (con las metricas del lazo); respuesta: bg_settings_t */
    BG_MSG_AUTH,          /* carga: bg_auth_msg_t (safety_mon -> planta); respuesta vacía o EPERM */
    BG_MSG_MONITOR,       /* respuesta: bg_monitor_t (hmi -> safety_mon) */
};

/* Autorización de actuadores en la planta (tarea 4, tareas/04_plant_auth). safety_mon le dice a pneumatic_sim
 * qué pid puede mandar WRITE: el del breath_ctrl que acaba de lanzar. La confianza no viene del pid escrito en
 * el mensaje, sino del pid del EMISOR, que pone el kernel en struct _msg_info. */
typedef struct {
    bg_hdr_t hdr;
    int32_t pid; /* proceso que podrá mandar WRITE */
} bg_auth_msg_t;

/* Metricas de tiempo real del lazo de breath_ctrl, desde que arranco esa instancia. Viajan en la consulta de
 * ajustes (cada 100 ms), asi no hace falta otro mensaje. Histograma del periodo en BG_HIST_BINS clases
 * logaritmicas (4 por octava) centradas en 1 ms: la clase i va de bg_hist_edge(i) a bg_hist_edge(i + 1); la
 * primera y la ultima recogen todo lo que queda fuera. */
#define BG_HIST_BINS 28
#define BG_HIST_CENTER 8 /* clase que contiene 1 ms */
#define BG_LATE_MS 1.5   /* un ciclo que empieza mas de 0.5 ms tarde cuenta como plazo incumplido */
static inline double bg_hist_edge(int i) { return 0.25 * __builtin_exp2((i - 0.5) / 4.0); }

typedef struct {
    int32_t pid;
    uint32_t cycles;      /* ciclos ejecutados */
    uint32_t late;        /* ciclos con periodo > BG_LATE_MS (plazo incumplido) */
    uint32_t lost;        /* periodos de 1 ms perdidos: suma de round(periodo) - 1 */
    float p50_ms, p99_ms, p999_ms, max_ms; /* periodo */
    float exec_p99_us, exec_max_us;        /* computo de un ciclo (leer + calcular + escribir) */
    uint32_t hist[BG_HIST_BINS];
} bg_loop_stats_t;

typedef struct {
    bg_hdr_t hdr;
    bg_loop_stats_t loop;
} bg_settings_get_t;

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
    double fio2;                 /* fracción de O2 del gas que respira el paciente en Pulse */
} bg_patient_state_t;

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
    double cmd_age_max;  /* s: antigüedad máxima del último comando aplicado desde la consulta anterior */
    double cmd_age_p99_all, cmd_age_max_all; /* s: p99 y máximo de la antigüedad desde el arranque (cada 1 ms) */
    double fio2;
    /* Comandos rechazados por la autorización (tarea 4): total, y el último emisor y tipo de mensaje. */
    uint32_t denied;
    int32_t denied_pid;
    uint32_t denied_type;
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
    bg_loop_stats_t loop;  /* ultimas metricas del lazo de breath_ctrl */
    double loop_age_s;     /* antigüedad de esas metricas (crece si el controlador no consulta) */
    double ibw_kg;         /* peso ideal del paciente */
    /* Limites de alarma activos */
    double lim_p_high, lim_p_low, lim_apnea_s, lim_vt_lo, lim_vt_hi, lim_auto_peep;
    /* Mecanica por respiracion (lung_mon): auto-PEEP estimado, flujo al final de la espiracion, tau espiratoria */
    double auto_peep, q_ee, tau_exp;
    int32_t eef;           /* 1 = el flujo no vuelve a cero al final de la espiracion */
    uint32_t denied;       /* comandos rechazados por la planta (tarea 4) */
} bg_safety_status_t;

/* Lo que la HMI ve de la planta, a través de safety_mon: la HMI ya no abre conexión con pneumatic_sim. */
typedef struct {
    sensors_t s;     /* la última lectura de safety_mon (cada 1 ms) */
    bg_vitals_t vt;  /* la última consulta de safety_mon a la planta (cada 100 ms) */
} bg_monitor_t;

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
