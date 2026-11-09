/* plant.c - física del circuito neumático y del pulmón. Ver plant.h. */
#include "plant.h"

#include <math.h>
#include <string.h>

#define SUBSTEP_S 50e-6

static double clamp(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }

/* Ruido gaussiano: xorshift64 + Box-Muller. Determinista, para poder repetir corridas. */
static double uniform(plant_t *pl)
{
    uint64_t x = pl->rng;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    pl->rng = x;
    return ((double)(x >> 11) + 0.5) / 9007199254740992.0; /* (0, 1) */
}

static double gauss(plant_t *pl)
{
    return sqrt(-2.0 * log(uniform(pl))) * cos(2.0 * M_PI * uniform(pl));
}

plant_params_t plant_default_params(void)
{
    plant_params_t p = {
        .pbl_max = 60.0, .tau_bl = 0.040,
        .r_in = 3.0, .c_tube = 0.0015, .r_ex = 2.0, .r_disc = 0.5,
        .r_pat = 22.4, .c_pat = 0.058, .v0 = 2.3,
        .noise_paw = 0.15, .noise_flow = 0.01,
    };
    return p;
}

void plant_init(plant_t *pl, const plant_params_t *p, double peep)
{
    memset(pl, 0, sizeof *pl);
    pl->p = *p;
    pl->f.obstruction = 1.0;
    pl->pbl = peep;
    pl->paw = peep;
    pl->v = p->v0 + p->c_pat * peep; /* pulmón en equilibrio con la PEEP */
    pl->paw_last_read = peep;
    pl->rng = 0x9E3779B97F4A7C15ULL;
}

double plant_palv(const plant_t *pl) { return (pl->v - pl->p.v0) / pl->p.c_pat; }

static void substep(plant_t *pl, const actuators_t *a, double h)
{
    const plant_params_t *p = &pl->p;
    const plant_faults_t *f = &pl->f;

    /* Soplador: primer orden hacia la presión pedida. */
    double target = p->pbl_max * clamp(a->blower, 0.0, 1.0);
    pl->pbl += (target - pl->pbl) * h / p->tau_bl;

    /* Rama inspiratoria con válvula antirretorno: el flujo solo va del soplador a la Y. */
    double q_in = pl->pbl > pl->paw ? (pl->pbl - pl->paw) / p->r_in : 0.0;

    /* Válvula espiratoria + válvula de PEEP: solo deja salir aire por encima de peep_cmd. */
    double exh = clamp(a->exh_open, 0.0, 1.0);
    double q_ex = pl->paw > a->peep_cmd ? exh * (pl->paw - a->peep_cmd) / p->r_ex : 0.0;

    double q_leak = f->g_leak * pl->paw;
    double palv = plant_palv(pl);
    double r_pat = p->r_pat * (f->obstruction > 1.0 ? f->obstruction : 1.0);

    double q_y, q_lung;
    if (f->disconnected) {
        q_y = pl->paw / p->r_disc; /* el circuito se vacía al aire por la Y */
        q_lung = -palv / r_pat;    /* el pulmón exhala al aire */
        q_leak = 0;
    } else {
        q_y = (pl->paw - palv) / r_pat;
        q_lung = q_y;
    }

    pl->paw_int += (f->disconnected ? 0.0 : pl->paw) * h; /* lo que "ve" la vía aérea del paciente */
    pl->paw += (q_in - q_ex - q_leak - q_y) * h / p->c_tube;
    pl->v += q_lung * h;
    pl->t += h;

    /* La fuga es del lado del paciente (manguito o mascarilla): sale entre el sensor de la Y y el pulmón, así que
     * el sensor de flujo la cuenta como flujo hacia el paciente. Por eso Vte < Vti cuando hay fuga. */
    pl->q_in = q_in; pl->q_ex = q_ex; pl->q_y = q_y + q_leak; pl->q_lung = q_lung;
}

void plant_advance(plant_t *pl, const actuators_t *a, double dt)
{
    while (dt > 1e-12) {
        double h = dt < SUBSTEP_S ? dt : SUBSTEP_S;
        substep(pl, a, h);
        dt -= h;
    }
}

void plant_sense(plant_t *pl, sensors_t *s)
{
    if (!pl->f.paw_stuck)
        pl->paw_last_read = pl->paw + pl->p.noise_paw * gauss(pl);
    s->seq = ++pl->seq;
    s->t_s = pl->t;
    s->paw = pl->paw_last_read;
    s->flow = pl->q_y + pl->p.noise_flow * gauss(pl);
}
