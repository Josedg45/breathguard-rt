# Tarea 5: recorrer el árbol de decisión del copiloto (`tree_predict`)

El copiloto es un proceso de baja prioridad que mira cada respiración y dice **qué cree que le pasa al paciente o
al circuito**: *normal*, *fuga*, *resistencia alta* o *pulmón rígido*. No toca nada. Su sugerencia pasa por
`safety_mon`, que la valida con límites fijos, y solo se aplica si el operador la confirma.

La parte "inteligente" es un **árbol de decisión** que se entrenó en Ubuntu (`scripts/copilot_train.py`, con
scikit-learn) usando ~19 000 respiraciones del banco de pruebas. Se exportó a C como unos arreglos de números
(`src/copilot_tree.h`). En QNX no hay Python ni scikit-learn: hay que **recorrer esos arreglos en C**. Esa es tu
tarea, en `tree_predict.c`: unas 25-35 líneas.

## Qué es un árbol de decisión

Es una cadena de preguntas de sí o no sobre las features de la respiración. Las preguntas no las escribimos
nosotros: el entrenamiento eligió **qué feature mirar y con qué umbral** en cada nodo, para separar mejor las
clases. Las primeras preguntas del árbol real (todo el árbol está en `results/copilot/tree_rules.txt`):

```
¿leak_frac <= 0.109?                      (¿se perdió menos del 11 % del volumen inspirado?)
├─ sí: ¿qei_rel <= 0.486?                 (¿el flujo casi llegó a cero al final de la inspiración?)
│      ├─ sí: ¿cdyn_ml <= 29.1?           (¿compliance dinámica baja?) ...
│      └─ no: ¿g_insp <= 0.027?           (¿poco flujo por cada cmH2O? = resistencia alta) ...
└─ no: ¿g_insp <= 0.021? ...
```

Cada camino termina en una **hoja**, que guarda qué fracción de las respiraciones de entrenamiento que llegaron
ahí era de cada clase. La respuesta es la clase con mayor fracción, y esa fracción es la **confianza**.

Comparación con lo que conoces: en Arduino escribirías estos `if` a mano. Aquí los `if` son **datos**: el mismo
código C recorre cualquier árbol, y para cambiar el modelo solo se reemplaza `copilot_tree.h`.

## Cómo está guardado (`src/copilot_tree.h`)

Arreglos paralelos: el nodo `i` es la posición `i` de todos.

| Arreglo | En un nodo de decisión | En una hoja |
|---|---|---|
| `feature[i]` | qué feature mirar (índice en `x`, ver `breath_feat.h`) | `-1` |
| `threshold[i]` | el umbral | sin uso |
| `left[i]`, `right[i]` | a qué nodo ir si `x <= umbral` / si no | `-1` |
| `value[i][k]` | sin uso | fracción de la clase `k` (suman 1) |

En `tree_predict.h` el árbol llega como `tp_tree_t` (punteros y tamaños). `value` es un arreglo plano:
la fracción de la clase `k` en el nodo `i` está en `value[i * n_classes + k]`.

## La trampa del float

scikit-learn convierte las features a `float` (32 bits) antes de comparar. Los umbrales los guarda en `double`,
a medio camino entre dos valores `float`. Si comparas el `double` original, en algún caso justo en el borde
irás por la rama contraria a la de Python. Para que el C diga **exactamente** lo mismo que el entrenamiento:

```c
(double)(float)x[f] <= t->threshold[i]
```

La prueba tiene un caso para esto (B13) y un vector "en el borde" por cada nodo (A2).

## Por qué tantos `return -1`

Este código corre en un dispositivo médico. Una respuesta "no sé" es segura, porque el copiloto simplemente no
sugiere nada. Una respuesta equivocada no lo es. Por eso:
- **NaN o infinito** en una feature que hay que mirar: con NaN, `x <= umbral` da *falso* y el recorrido se va a
  la derecha sin avisar. Se devuelve -1.
- **Árbol dañado** (índice fuera de rango, feature inválida, ciclo): nunca se lee fuera de un arreglo y nunca se
  queda en un bucle infinito. Un camino real nunca es más largo que `n_nodes`.

## Prueba

```
make tarea5-host     # en Ubuntu, rápido
make tarea5          # en la VM QNX (la que cuenta)
```

- **Parte A:** el árbol real contra las respuestas de scikit-learn en 1500 respiraciones que no se usaron para
  entrenar, más un vector en el borde de cada nodo de decisión.
- **Parte B:** árboles pequeños hechos a mano: izquierda/derecha/igual, NaN, infinito, hijos fuera de rango,
  ciclo, feature inválida, árbol vacío, empate y la trampa del float.

Con el esqueleto fallan 8. Tiene que decir `PRUEBA TAREA 5: PASA`.

## Preguntas (respóndelas en APRENDIZAJE.md)

1. ¿Cuántas comparaciones hace como máximo `tree_predict` con el árbol real (profundidad 5)? ¿Por qué eso
   importa en un sistema de tiempo real, comparado con una red neuronal?
2. Con NaN, ¿por qué la comparación `x <= umbral` manda siempre a la derecha? ¿Qué clase saldría sin la
   comprobación `isfinite`?
3. El árbol dice "fuga" con confianza 0.99. ¿Eso quiere decir que hay una fuga con un 99 % de probabilidad?
   (Pista: ¿de dónde salen esas fracciones?)
4. ¿Qué parte del copiloto es aprendida y qué parte son reglas fijas? ¿Cuál puede cambiar sin volver a validar
   la seguridad del ventilador?
