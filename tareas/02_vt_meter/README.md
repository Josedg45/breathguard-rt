# Tarea 2: el medidor de volumen corriente (`vt_meter`)

El ventilador mide el flujo en la pieza en Y, pero las alarmas y la pantalla necesitan **volumen**: cuánto aire
entró (Vti) y cuánto salió (Vte) en cada respiración. Se obtiene integrando el flujo, como integrar la corriente
para obtener la carga de un capacitor.

Si Vte es mucho menor que Vti, el aire se está escapando: hay una fuga o el tubo está desconectado. Por eso
`safety_mon` (M4) usará este medidor para las alarmas de volumen bajo, volumen alto y fuga.

Tu parte es `vtm_update()` en `vt_meter.c`: unas 25-35 líneas.

## Reglas
1. **Integración:** entre la muestra anterior y la actual, suma el área bajo el flujo con la **regla del
   trapecio**: `(q_anterior + q_actual) / 2 · dt`. El `dt` es el **real** (`t - t_anterior`), nunca 1 ms
   supuesto. Recuerda M1: en la VM hay ciclos de 2, 10 y hasta 40 ms.
2. **Fin de la inspiración** (`insp` pasa de 1 a 0): primero integra el intervalo que acaba de terminar
   (regla 1) y luego guarda `vti` = volumen integrado desde el inicio de esta respiración.
3. **Inicio de una inspiración nueva** (`insp` pasa de 0 a 1): primero integra el intervalo; si ya había una
   respiración en curso, `vte` = volumen al final de su inspiración − volumen actual (en positivo) y suma 1 a
   `breaths`. Después, el volumen vuelve a 0 para la respiración nueva.
4. **Huecos:** si `dt` es mayor que `VTM_MAX_GAP_S` (50 ms), no integres ese intervalo (no sabes qué pasó en
   medio) y suma 1 a `gaps`.
5. **Tiempo que no avanza:** si `dt <= 0` (muestra repetida o reloj hacia atrás), no integres y suma 1 a
   `gaps`. Esa muestra sí pasa a ser la "anterior".
6. **Arranque:** la primera respiración empieza en la primera muestra con `insp = 1`, sea porque `insp` pasó de
   0 a 1 o porque la primera muestra ya era de inspiración. Lo de antes no cuenta.

## Antes de programar
Responde con tus palabras, y mándamelo junto con el código:
1. ¿Por qué trapecio y no simplemente `q · dt`? ¿Cuándo da lo mismo?
2. ¿Qué pasaría con el Vti medido si usaras `dt = 1 ms` fijo en una corrida con la CPU saturada (M3 perdió hasta el
   18 % de los ticks)? ¿Más o menos volumen que el real?
3. Si el tubo se desconecta durante la espiración, ¿qué esperas ver en Vte? ¿Y en Vti de la respiración siguiente?

## Cómo probarlo
Desde la raíz del repo, con el entorno de QNX cargado y la VM encendida:
```bash
make tarea2
```
Compila, copia a la VM y ejecuta la prueba. Con el esqueleto sin completar **debe fallar**.
Cuando termine en `RESULTADO: todas las pruebas pasaron`, avísame para revisarlo.

Pista para empezar: dibuja en papel tres muestras (t, q, insp) que crucen de inspiración a espiración, y escribe a
mano qué debe pasar en cada llamada. Si te atascas, pide una pista antes que la solución.
