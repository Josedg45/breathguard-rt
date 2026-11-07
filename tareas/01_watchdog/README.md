# Tarea 1: el watchdog de `safety_mon`

Si `breath_ctrl` se cae o se cuelga, el paciente deja de ventilarse. El watchdog es lo que lo detecta:
`breath_ctrl` envía un "latido" periódico con un contador, y si deja de llegar a tiempo, `safety_mon`
lleva el sistema a estado seguro. Es como el watchdog de un ESP32, pero vigilando otro proceso.

Tu parte es la lógica de decisión, en `watchdog.c`: unas 30-40 líneas. Cuando pase la prueba, la
integramos en `safety_mon` con las APIs de QNX.

## Reglas
1. **Arranque:** tras armarse queda en `WD_WAITING`. Si no llega ningún latido válido y el tiempo desde
   que se armó **supera** `startup_ns`, pasa a `WD_TIMEOUT`.
2. **Latido válido:** el primero siempre vale, sea cual sea su secuencia, y pasa a `WD_OK`. Después,
   solo vale si su secuencia es **distinta** de la anterior. Uno repetido se ignora por completo.
3. **Plazo:** en `WD_OK`, si el tiempo desde el último latido válido **supera** `timeout_ns`, pasa a
   `WD_TIMEOUT`. Justo en el límite todavía está bien.
4. **El fallo se queda:** una vez en `WD_TIMEOUT`, los latidos no cambian nada. Solo `wd_arm` lo reinicia.
5. **Tiempos fuera de orden:** si `now_ns` es anterior al último latido, no hay timeout.

## Antes de programar
Responde con tus palabras, y mándamelo junto con el código:
1. ¿Por qué se envía un contador y no solo un "estoy vivo"? ¿Qué fallo real detecta?
2. ¿Por qué un latido tardío no debe devolver el sistema a `WD_OK`? Piensa en lo que ya hizo `safety_mon`.
3. Si `timeout_ns` es 50 ms y el lazo corre a 1 kHz, ¿qué pasa si lo bajas a 2 ms en esta VM? Recuerda M1.

## Cómo probarlo
Desde la raíz del repo, con el entorno de QNX cargado y la VM encendida:
```bash
make tarea1
```
Compila, copia a la VM y ejecuta la prueba. Con el esqueleto sin completar **debe fallar**.
Cuando termine en `RESULTADO: todas las pruebas pasaron`, avísame para revisarlo.
Pasar la prueba es necesario, pero no basta: también revisaré tu código y tus respuestas.

Si te atascas, pídeme una pista antes que la solución.
