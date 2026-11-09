/* tree_predict.c - TAREA 5 (tuya). Lee README.md antes de empezar. Unas 25-35 líneas.
 *
 * Mientras devuelva -1, el copiloto nunca sugiere nada (es seguro, pero no sirve para nada).
 * Prueba: make tarea5   (en Ubuntu: make tarea5-host) */
#include "tree_predict.h"

#include <math.h>

int tree_predict(const tp_tree_t *t, const double *x, double *conf)
{
    *conf = 0.0;
    (void)t;
    (void)x;

    /* TODO 1: empieza en la raíz (nodo 0) y recorre como máximo t->n_nodes pasos.
     *         Si el índice del nodo actual no está en [0, n_nodes), el árbol está dañado: return -1. */

    /* TODO 2: si el nodo es una hoja (feature == -1), sal del recorrido.
     *         Si la feature no está en [0, n_features), árbol dañado: return -1. */

    /* TODO 3: lee x[feature]. Si no es un número finito (isfinite), return -1: no se adivina con datos malos.
     *         Compara como scikit-learn (README, "La trampa del float") y avanza a left o right. */

    /* TODO 4: si saliste por el límite de pasos sin llegar a una hoja, return -1 (hay un ciclo).
     *         En la hoja: busca la clase con mayor value (si empatan, la de menor índice),
     *         pon su fracción en *conf y devuélvela. */

    return -1;
}
