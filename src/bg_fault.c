/* bg_fault.c - inyecta fallas en pneumatic_sim (CLI de pruebas). Imprime el instante de la inyección
 * con el mismo reloj (ClockCycles) que usan los demás procesos, para medir la latencia de las alarmas.
 *
 * Uso: bg_fault [-d 0|1] [-o factor] [-l conductancia] [-k 0|1] [-w segundos]
 *   -d  tubo desconectado (1) o conectado (0)
 *   -o  obstrucción: multiplica la resistencia del paciente (1 = normal)
 *   -l  fuga: conductancia en L/s por cmH2O (0 = sin fuga)
 *   -k  sensor de presión congelado (1) o normal (0)
 *   -w  espera estos segundos antes de inyectar
 * Sin opciones: quita todas las fallas. */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/dispatch.h>
#include <unistd.h>

#include "bg_proto.h"
#include "rt_util.h"

#define PROG "bg_fault"

int main(int argc, char **argv)
{
    bg_fault_msg_t m = { .hdr = { .type = BG_MSG_FAULT }, .faults = { .obstruction = 1.0 } };
    double wait = 0;
    int opt;
    while ((opt = getopt(argc, argv, "d:o:l:k:w:")) != -1) {
        switch (opt) {
        case 'd': m.faults.disconnected = atoi(optarg); break;
        case 'o': m.faults.obstruction = atof(optarg); break;
        case 'l': m.faults.g_leak = atof(optarg); break;
        case 'k': m.faults.paw_stuck = atoi(optarg); break;
        case 'w': wait = atof(optarg); break;
        default:
            fprintf(stderr, "uso: %s [-d 0|1] [-o factor] [-l conductancia] [-k 0|1] [-w segundos]\n", PROG);
            return 1;
        }
    }
    int coid = name_open(BG_PNEUMATIC_NAME, 0);
    if (coid == -1) rt_die(PROG, "name_open(" BG_PNEUMATIC_NAME ")");
    if (wait > 0) usleep((useconds_t)(wait * 1e6));

    double t = rt_now_s();
    if (MsgSend(coid, &m, sizeof m, NULL, 0) == -1) rt_die(PROG, "MsgSend(FAULT)");
    printf("%s: t_abs=%.6f desconectado=%d obstruccion=%.1f fuga=%.3f sensor_congelado=%d\n", PROG, t,
           m.faults.disconnected, m.faults.obstruction, m.faults.g_leak, m.faults.paw_stuck);
    name_close(coid);
    return 0;
}
