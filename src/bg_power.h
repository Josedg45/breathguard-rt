/* bg_power.h - mensajes QNX de power_mgr (M5). */
#ifndef BG_POWER_H
#define BG_POWER_H

#include <stdint.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>

#include "bg_proto.h"

#define BG_POWER_NAME "breathguard_power"

enum {
    BG_MSG_MAINS = 0x4100,   /* carga: int32 on (1 = hay red); respuesta vacía (inyección de fallas) */
    BG_MSG_POWER_STATUS,     /* respuesta: bg_power_status_t */
};

/* Pulso de power_mgr a safety_mon con los bits de estado de energía en value. */
#define BG_PULSE_POWER (_PULSE_CODE_MINAVAIL + 3)
enum {
    PWR_ON_BATTERY = 1u << 0,  /* se cortó la red */
    PWR_BATTERY_LOW = 1u << 1, /* SoC < 20 % o autonomía < 30 min */
    PWR_BATTERY_CRIT = 1u << 2,/* SoC < 5 % */
    PWR_SHEDDING = 1u << 3,    /* consumos no esenciales recortados */
};

typedef struct {
    bg_hdr_t hdr;
    int32_t on;
} bg_mains_msg_t;

typedef struct {
    uint32_t bits;
    double soc, v_batt, i_batt, p_load_w, autonomy_s;
} bg_power_status_t;

#endif
