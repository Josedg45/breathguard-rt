/* tree_predict.c - tarea 5: recorrer el árbol de decisión del copiloto. Las reglas están en README.md;
 * la prueba es test_tree_predict.c (make tarea5 en la VM, make tarea5-host en Ubuntu).
 *
 * Tiempo acotado: como mucho n_nodes pasos (con el árbol real, 5 comparaciones), sin memoria dinámica y sin
 * recursión. Ante cualquier duda devuelve -1 ("no sé"), que para el copiloto significa "no sugerir nada". */
#include "tree_predict.h"

#include <math.h>

int tree_predict(const tp_tree_t *t, const double *x, double *conf)
{
    *conf = 0.0;
    if (t->n_nodes <= 0 || t->n_classes <= 0 || t->n_classes > TP_MAX_CLASSES)
        return -1;                                  /* árbol vacío o mal declarado */

    int node = 0;
    int steps;
    for (steps = 0; steps < t->n_nodes; steps++) {
        if (node < 0 || node >= t->n_nodes)
            return -1;                              /* hijo fuera de rango: árbol dañado */
        int f = t->feature[node];
        if (f == -1)
            break;                                  /* hoja */
        if (f < 0 || f >= t->n_features)
            return -1;                              /* feature inválida */
        if (!isfinite(x[f]))
            return -1;                              /* NaN o infinito: no se adivina con datos malos */
        /* scikit-learn compara la feature convertida a float (32 bits) contra un umbral double. */
        node = ((double)(float)x[f] <= t->threshold[node]) ? t->left[node] : t->right[node];
    }
    /* Si se agotaron los pasos sin llegar a una hoja hay un ciclo. Si el último salto fue al nodo
     * n_nodes (o más), tampoco hay hoja válida. Ambos casos se detectan aquí. */
    if (steps == t->n_nodes || node < 0 || node >= t->n_nodes || t->feature[node] != -1)
        return -1;

    const float *v = &t->value[node * t->n_classes];
    int best = 0;
    for (int k = 1; k < t->n_classes; k++)
        if (v[k] > v[best])                         /* estricto: en empate gana el menor índice */
            best = k;
    *conf = v[best];
    return best;
}
