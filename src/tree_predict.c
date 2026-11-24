/* tree_predict.c - walks the copilot's decision tree. Test: tests/test_tree_predict.c.
 *
 * Bounded time: at most n_nodes steps (5 comparisons with the real tree), no dynamic memory and no recursion.
 * When in doubt it returns -1 ("don't know"), which for the copilot means "suggest nothing". */
#include "tree_predict.h"

#include <math.h>

int tree_predict(const tp_tree_t *t, const double *x, double *conf)
{
    *conf = 0.0;
    if (t->n_nodes <= 0 || t->n_classes <= 0 || t->n_classes > TP_MAX_CLASSES)
        return -1;                                  /* empty or badly declared tree */

    int node = 0;
    int steps;
    for (steps = 0; steps < t->n_nodes; steps++) {
        if (node < 0 || node >= t->n_nodes)
            return -1;                              /* child out of range: damaged tree */
        int f = t->feature[node];
        if (f == -1)
            break;                                  /* leaf */
        if (f < 0 || f >= t->n_features)
            return -1;                              /* invalid feature */
        if (!isfinite(x[f]))
            return -1;                              /* NaN or infinity: no guessing on bad data */
        /* scikit-learn compares the feature converted to float (32 bits) against a double threshold. */
        node = ((double)(float)x[f] <= t->threshold[node]) ? t->left[node] : t->right[node];
    }
    /* Running out of steps without reaching a leaf means a cycle. A last jump to node n_nodes (or beyond)
     * is not a valid leaf either. Both cases are caught here. */
    if (steps == t->n_nodes || node < 0 || node >= t->n_nodes || t->feature[node] != -1)
        return -1;

    const float *v = &t->value[node * t->n_classes];
    int best = 0;
    for (int k = 1; k < t->n_classes; k++)
        if (v[k] > v[best])                         /* strict: on a tie the lowest index wins */
            best = k;
    *conf = v[best];
    return best;
}
