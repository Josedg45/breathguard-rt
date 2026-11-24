/* tree_predict.h - walks the decision tree exported by scripts/copilot_train.py.
 * Plain C, no QNX: tested the same way on Ubuntu and on the VM (tests/test_tree_predict.c). */
#ifndef TREE_PREDICT_H
#define TREE_PREDICT_H

#include <stdint.h>

#define TP_MAX_CLASSES 8

/* A tree as parallel arrays (see src/copilot_tree.h). Node i:
 *   feature[i] == -1  -> leaf: value[i][k] = fraction of class k in that leaf (they add up to 1).
 *   feature[i] >= 0   -> if x[feature[i]] <= threshold[i] (compared as scikit-learn does: the feature
 *                        converted to float32 against a double threshold) go to left[i]; otherwise right[i].
 * Node 0 is the root. */
typedef struct {
    int n_nodes, n_features, n_classes;
    const int16_t *feature;
    const double *threshold;
    const int16_t *left, *right;
    const float *value;        /* n_nodes * n_classes: value[i * n_classes + k] */
} tp_tree_t;

/* Walks the tree with features x[0..n_features-1].
 * Returns the leaf's class (largest fraction; on a tie, the lowest index) and that fraction in *conf.
 * Returns -1 (and *conf = 0) when no reliable answer is possible: NaN or infinity in a feature that must be
 * read, or a damaged tree (index out of range, invalid feature, a path longer than n_nodes). */
int tree_predict(const tp_tree_t *t, const double *x, double *conf);

#endif
