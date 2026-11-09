/* tree_predict.h - tarea 5: recorrer el árbol de decisión exportado por scripts/copilot_train.py.
 * C puro, sin QNX: se prueba igual en Ubuntu que en la VM. */
#ifndef TREE_PREDICT_H
#define TREE_PREDICT_H

#include <stdint.h>

#define TP_MAX_CLASSES 8

/* Un árbol como arreglos paralelos (ver src/copilot_tree.h). Nodo i:
 *   feature[i] == -1  -> hoja: value[i][k] = fracción de la clase k en esa hoja (suman 1).
 *   feature[i] >= 0   -> si x[feature[i]] <= threshold[i] (comparado como lo hace scikit-learn, ver README)
 *                        se sigue por left[i]; si no, por right[i].
 * El nodo 0 es la raíz. */
typedef struct {
    int n_nodes, n_features, n_classes;
    const int16_t *feature;
    const double *threshold;
    const int16_t *left, *right;
    const float *value;        /* n_nodes * n_classes: value[i * n_classes + k] */
} tp_tree_t;

/* Recorre el árbol con las features x[0..n_features-1].
 * Devuelve la clase de la hoja (la de mayor fracción; si empatan, la de menor índice) y, en *conf, esa fracción.
 * Devuelve -1 (y *conf = 0) si no puede dar una respuesta confiable: x con NaN o infinito en una feature que
 * hay que mirar, o un árbol dañado (índice fuera de rango, feature inválida, un camino más largo que n_nodes). */
int tree_predict(const tp_tree_t *t, const double *x, double *conf);

#endif
