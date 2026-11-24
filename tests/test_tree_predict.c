/* test_tree_predict.c - unit test of src/tree_predict.c.
 *   Part A: the copilot's real tree (src/copilot_tree.h) against scikit-learn's answers on breaths the tree did
 *           not see in training, plus one vector "on the edge" of each decision node.
 *   Part B: small hand-made trees for the safety cases (bad data, damaged tree, ties). */
#include <math.h>
#include <stdio.h>

#include "copilot_tree.h"
#include "test_vectors.h"
#include "tree_predict.h"

static int fails;

static void check(const char *name, int got, int want, double conf, double want_conf)
{
    int ok = got == want && fabs(conf - want_conf) < 1e-5;
    printf("%-62s %s  (expects class %d conf %.2f; got %d conf %.2f)\n", name, ok ? "OK  " : "FAIL", want, want_conf,
           got, conf);
    if (!ok) fails++;
}

/* 3-node tree: x[f] <= thr ? leaf 1 : leaf 2. */
static const float V3[3][2] = { { 0, 0 }, { 0.9f, 0.1f }, { 0.2f, 0.8f } };

static tp_tree_t tree3(const int16_t *feat, const double *thr, const int16_t *l, const int16_t *r)
{
    tp_tree_t t = { .n_nodes = 3, .n_features = 2, .n_classes = 2, .feature = feat, .threshold = thr, .left = l,
                    .right = r, .value = &V3[0][0] };
    return t;
}

int main(void)
{
    /* ---------------- Part A ---------------- */
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
                printf("  vector %d: expects class %d conf %.4f; got %d conf %.4f\n", i, tv_class[i], tv_conf[i], c,
                       conf);
        }
    }
    printf("%-62s %s  (%d of %d differ)\n", "A1 unseen breaths = scikit-learn", bad_rand ? "FAIL" : "OK  ",
           bad_rand, TV_N - TV_N_EDGE);
    printf("%-62s %s  (%d of %d differ)\n", "A2 feature exactly at the threshold = scikit-learn",
           bad_edge ? "FAIL" : "OK  ", bad_edge, TV_N_EDGE);
    fails += (bad_rand > 0) + (bad_edge > 0);

    /* ---------------- Part B ---------------- */
    const int16_t F[3] = { 0, -1, -1 }, L[3] = { 1, -1, -1 }, R[3] = { 2, -1, -1 };
    const double T[3] = { 1.0, -2, -2 };
    tp_tree_t t = tree3(F, T, L, R);
    double conf, x[2];
    int c;

    x[0] = 0.5; x[1] = 0;
    c = tree_predict(&t, x, &conf); check("B1 x <= threshold goes left", c, 0, conf, 0.9);
    x[0] = 1.5;
    c = tree_predict(&t, x, &conf); check("B2 x > threshold goes right", c, 1, conf, 0.8);
    x[0] = 1.0;
    c = tree_predict(&t, x, &conf); check("B3 x == threshold goes left (<=)", c, 0, conf, 0.9);
    x[0] = NAN;
    c = tree_predict(&t, x, &conf); check("B4 NaN in a feature that is read -> -1", c, -1, conf, 0);
    x[0] = INFINITY;
    c = tree_predict(&t, x, &conf); check("B5 infinity -> -1", c, -1, conf, 0);
    x[0] = 0.5; x[1] = NAN;
    c = tree_predict(&t, x, &conf); check("B6 NaN in a feature that is NOT read -> answers", c, 0, conf, 0.9);
    x[1] = 0;

    const int16_t Lbad[3] = { 7, -1, -1 };
    tp_tree_t tb = tree3(F, T, Lbad, R);
    c = tree_predict(&tb, x, &conf); check("B7 child out of range -> -1", c, -1, conf, 0);
    const int16_t Lneg[3] = { -1, -1, -1 };
    tb = tree3(F, T, Lneg, R);
    c = tree_predict(&tb, x, &conf); check("B8 child -1 in a decision node -> -1", c, -1, conf, 0);
    const int16_t Lcyc[3] = { 0, -1, -1 };
    tb = tree3(F, T, Lcyc, R);
    c = tree_predict(&tb, x, &conf); check("B9 cycle (child points to the root) -> -1", c, -1, conf, 0);
    const int16_t Fbad[3] = { 5, -1, -1 };
    tb = tree3(Fbad, T, L, R);
    c = tree_predict(&tb, x, &conf); check("B10 feature out of range -> -1", c, -1, conf, 0);
    tb = tree3(F, T, L, R);
    tb.n_nodes = 0;
    c = tree_predict(&tb, x, &conf); check("B11 empty tree -> -1", c, -1, conf, 0);

    static const float Vtie[1][3] = { { 0.4f, 0.2f, 0.4f } };
    const int16_t F1[1] = { -1 }, N1[1] = { -1 };
    const double T1[1] = { -2 };
    tp_tree_t leaf = { .n_nodes = 1, .n_features = 2, .n_classes = 3, .feature = F1, .threshold = T1, .left = N1,
                       .right = N1, .value = &Vtie[0][0] };
    c = tree_predict(&leaf, x, &conf); check("B12 single leaf, tie 0.4/0.4 -> lowest index", c, 0, conf, 0.4);

    /* B13: the float trap. A threshold halfway between two consecutive floats a < b, chosen so that
     * (float)threshold rounds to b. As a double, x = threshold satisfies x <= threshold; as a float, (float)x = b > threshold. */
    float a = 0.1f;
    for (;;) {
        float b = nextafterf(a, 1.0f);
        double mid = ((double)a + (double)b) / 2.0;
        if ((float)mid == b) {
            const double Tf[3] = { mid, -2, -2 };
            tp_tree_t tf = tree3(F, Tf, L, R);
            x[0] = mid;
            c = tree_predict(&tf, x, &conf);
            check("B13 compares as float (like scikit-learn)", c, 1, conf, 0.8);
            break;
        }
        a = b;
    }

    printf(fails ? "\nRESULT: %d check(s) failed\n" : "\nRESULT: all checks passed\n", fails);
    return fails ? 1 : 0;
}
