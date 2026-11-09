/* test_tree_predict.c - prueba de la tarea 5. No la modifiques.
 *   Parte A: el árbol real del copiloto (src/copilot_tree.h) contra las respuestas de scikit-learn en respiraciones
 *            que el árbol no vio en el entrenamiento, más un vector "en el borde" por cada nodo de decisión.
 *   Parte B: árboles pequeños hechos a mano para los casos de seguridad (datos malos, árbol dañado, empates). */
#include <math.h>
#include <stdio.h>

#include "copilot_tree.h"
#include "test_vectors.h"
#include "tree_predict.h"

static int fails;

static void check(const char *name, int got, int want, double conf, double want_conf)
{
    int ok = got == want && fabs(conf - want_conf) < 1e-5;
    printf("%-62s %s  (espera clase %d conf %.2f; dio %d conf %.2f)\n", name, ok ? "OK  " : "FALLA", want, want_conf,
           got, conf);
    if (!ok) fails++;
}

/* Árbol de 3 nodos: x[f] <= thr ? hoja 1 : hoja 2. */
static const float V3[3][2] = { { 0, 0 }, { 0.9f, 0.1f }, { 0.2f, 0.8f } };

static tp_tree_t tree3(const int16_t *feat, const double *thr, const int16_t *l, const int16_t *r)
{
    tp_tree_t t = { .n_nodes = 3, .n_features = 2, .n_classes = 2, .feature = feat, .threshold = thr, .left = l,
                    .right = r, .value = &V3[0][0] };
    return t;
}

int main(void)
{
    /* ---------------- Parte A ---------------- */
    tp_tree_t real = { .n_nodes = CT_N_NODES, .n_features = CT_N_FEATURES, .n_classes = CT_N_CLASSES,
                       .feature = ct_feature, .threshold = ct_threshold, .left = ct_left, .right = ct_right,
                       .value = &ct_value[0][0] };
    int bad_rand = 0, bad_edge = 0;
    for (int i = 0; i < TV_N; i++) {
        double conf;
        int c = tree_predict(&real, tv_x[i], &conf);
        if (c != tv_class[i] || fabs(conf - tv_conf[i]) > 1e-5) {
            if (i < TV_N - TV_N_EDGE) bad_rand++;
            else bad_edge++;
            if (bad_rand + bad_edge <= 3)
                printf("  vector %d: espera clase %d conf %.4f; dio %d conf %.4f\n", i, tv_class[i], tv_conf[i], c,
                       conf);
        }
    }
    printf("%-62s %s  (%d de %d distintos)\n", "A1 respiraciones nuevas = scikit-learn", bad_rand ? "FALLA" : "OK  ",
           bad_rand, TV_N - TV_N_EDGE);
    printf("%-62s %s  (%d de %d distintos)\n", "A2 feature justo en el umbral = scikit-learn",
           bad_edge ? "FALLA" : "OK  ", bad_edge, TV_N_EDGE);
    fails += (bad_rand > 0) + (bad_edge > 0);

    /* ---------------- Parte B ---------------- */
    const int16_t F[3] = { 0, -1, -1 }, L[3] = { 1, -1, -1 }, R[3] = { 2, -1, -1 };
    const double T[3] = { 1.0, -2, -2 };
    tp_tree_t t = tree3(F, T, L, R);
    double conf, x[2];
    int c;

    x[0] = 0.5; x[1] = 0;
    c = tree_predict(&t, x, &conf); check("B1 x <= umbral va a la izquierda", c, 0, conf, 0.9);
    x[0] = 1.5;
    c = tree_predict(&t, x, &conf); check("B2 x > umbral va a la derecha", c, 1, conf, 0.8);
    x[0] = 1.0;
    c = tree_predict(&t, x, &conf); check("B3 x == umbral va a la izquierda (<=)", c, 0, conf, 0.9);
    x[0] = NAN;
    c = tree_predict(&t, x, &conf); check("B4 NaN en la feature que se mira -> -1", c, -1, conf, 0);
    x[0] = INFINITY;
    c = tree_predict(&t, x, &conf); check("B5 infinito -> -1", c, -1, conf, 0);
    x[0] = 0.5; x[1] = NAN;
    c = tree_predict(&t, x, &conf); check("B6 NaN en una feature que NO se mira -> responde", c, 0, conf, 0.9);
    x[1] = 0;

    const int16_t Lbad[3] = { 7, -1, -1 };
    tp_tree_t tb = tree3(F, T, Lbad, R);
    c = tree_predict(&tb, x, &conf); check("B7 hijo fuera de rango -> -1", c, -1, conf, 0);
    const int16_t Lneg[3] = { -1, -1, -1 };
    tb = tree3(F, T, Lneg, R);
    c = tree_predict(&tb, x, &conf); check("B8 hijo -1 en un nodo de decisión -> -1", c, -1, conf, 0);
    const int16_t Lcyc[3] = { 0, -1, -1 };
    tb = tree3(F, T, Lcyc, R);
    c = tree_predict(&tb, x, &conf); check("B9 ciclo (el hijo apunta a la raíz) -> -1", c, -1, conf, 0);
    const int16_t Fbad[3] = { 5, -1, -1 };
    tb = tree3(Fbad, T, L, R);
    c = tree_predict(&tb, x, &conf); check("B10 feature fuera de rango -> -1", c, -1, conf, 0);
    tb = tree3(F, T, L, R);
    tb.n_nodes = 0;
    c = tree_predict(&tb, x, &conf); check("B11 árbol vacío -> -1", c, -1, conf, 0);

    static const float Vtie[1][3] = { { 0.4f, 0.2f, 0.4f } };
    const int16_t F1[1] = { -1 }, N1[1] = { -1 };
    const double T1[1] = { -2 };
    tp_tree_t leaf = { .n_nodes = 1, .n_features = 2, .n_classes = 3, .feature = F1, .threshold = T1, .left = N1,
                       .right = N1, .value = &Vtie[0][0] };
    c = tree_predict(&leaf, x, &conf); check("B12 solo una hoja, empate 0.4/0.4 -> la de menor índice", c, 0, conf, 0.4);

    /* B13: la trampa del float. Umbral a medio camino entre dos float consecutivos a < b, elegido para que
     * (float)umbral redondee a b. Como double, x = umbral cumple x <= umbral; como float, (float)x = b > umbral. */
    float a = 0.1f;
    for (;;) {
        float b = nextafterf(a, 1.0f);
        double mid = ((double)a + (double)b) / 2.0;
        if ((float)mid == b) {
            const double Tf[3] = { mid, -2, -2 };
            tp_tree_t tf = tree3(F, Tf, L, R);
            x[0] = mid;
            c = tree_predict(&tf, x, &conf);
            check("B13 compara como float (igual que scikit-learn)", c, 1, conf, 0.8);
            break;
        }
        a = b;
    }

    printf("\nPRUEBA TAREA 5: %s (%d fallas)\n", fails ? "FALLA" : "PASA", fails);
    return fails ? 1 : 0;
}
