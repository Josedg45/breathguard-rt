# Tarea 3: límite de cambio por comando (`settings_rate_ok`)

Desde M6 el operador cambia los ajustes en marcha (`scripts/bg_cmd.py p_insp=25`). `safety_mon` ya rechaza
valores fuera de rango (por ejemplo, p_insp 50). Pero un valor válido puede ser un salto peligroso: pasar de
p_insp 10 a 35 en una sola respiración es un error típico de dedo (escribir 35 en vez de 15). Los ventiladores
reales piden confirmar los cambios grandes o los hacen por pasos.

Tu parte es `settings_rate_ok()` en `rate_limit.c`: unas 15-25 líneas. Hasta que la completes acepta todo y el
sistema funciona igual, sin este límite.

## Reglas (constantes en `rate_limit.c`, defínelas tú)
| Ajuste | Cambio máximo por comando |
|---|---|
| `RATE_P_INSP` (solo PCV) | 5 cmH2O |
| `RATE_VT` (solo VCV) | 0.1 L (100 mL) |
| `RATE_PEEP` | 3 cmH2O |
| `RATE_RR` | 4 rpm |

- Igual al límite **se acepta** (`<=`).
- Si cambia el modo (PCV ↔ VCV), p_insp y Vt no se comparan (son de modos distintos), pero PEEP y rr sí.
- I:E no tiene límite de cambio.
- Al rechazar, escribe el motivo en `why` con `snprintf(why, n, ...)`: la HMI se lo muestra al operador.
- Cuidado con el punto flotante: compara con `fabs(a - b) > LIMITE + 1e-9`, no con `==`.

## Antes de programar
Responde con tus palabras:
1. ¿Por qué la validación vive en `safety_mon` y no en la HMI, si la HMI es la que recibe el comando?
2. Un operador quiere ir de p_insp 15 a 30. ¿Cómo lo hace con este límite? ¿Cuánto tarda como mínimo, si cada
   cambio se aplica al inicio de la respiración siguiente y la frecuencia es 12 rpm?
3. ¿Qué pasaría si el límite se calculara contra los ajustes *programados* y no contra los vigentes? (Pista: en
   `safety_mon.c`, mira qué pasa como `cur` cuando ya hay un cambio pendiente.)

## Cómo probarlo
```bash
make tarea3
```
Con el esqueleto **debe fallar** (acepta saltos que debería rechazar).
