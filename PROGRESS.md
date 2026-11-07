# BreathGuard RT — Progreso

Entorno de todas las mediciones: QNX SDP 8.0, QEMU x86_64 con KVM (2 vCPU, 1 GB) sobre
WSL2/Windows, toolchain `qcc -Vgcc_ntox86_64`. Tick del sistema: 1 ms.

Cambio de enfoque (2026-11-06): el proyecto pasó de MedPower (continuidad eléctrica) a BreathGuard RT
(controlador de ventilador con paciente virtual). M1 se conserva porque no depende del producto.

| Hito | Contenido | Estado |
|---|---|---|
| M1 | Prueba de latencia | Completado |
| M2 | Pulse en el host, API y paso de tiempo, port a QNX | Completado |
| M3 | `pneumatic_sim` + `breath_ctrl` (PCV, VCV) | Pendiente |
| M4 | `safety_mon` | Pendiente |
| M5 | `power_mgr` | Pendiente |
| M6 | HMI, dashboard, CLI de fallos, versión monolítica | Pendiente |
| M7 | Entrega | Pendiente |

## M1 — Prueba de latencia (completado)

### Qué se hizo
- `src/latency_test.c`: timer de QNX a 1 kHz que entrega pulsos a un canal; hilo `SCHED_FIFO` que
  marca el tiempo con `ClockCycles()`. El CSV se escribe al final para no contaminar la medición.
- `src/cpu_burn.c`: carga de un hilo FIFO por vCPU; reporta el % de CPU que recibió cada hilo.
- `src/vm_noise.c`: detector de huecos a prio 200, para separar el ruido del host del de QNX.
- `scripts/m1_run.sh` (host) + `scripts/m1_target.sh` (VM): campaña completa y reproducible.
- `scripts/m1_check.py`: recalcula en Python todas las estadísticas desde las muestras crudas y
  falla si no coinciden con lo que reportó QNX.
- Datos en `results/m1/`: una muestra por ciclo en `<corrida>.csv`, y `summary.csv`, `noise_summary.csv`.

Definiciones: `jitter = |periodo − 1000 µs|`; `overrun = periodo > 1500 µs`.

### Resultados
Lazo a prio 50, 10 s por corrida (10 000 muestras), 3 repeticiones. Sin anclar a una CPU.
Promedio y p99 son la media de las 3 corridas; el máximo es el peor de las 3. El jitter mínimo fue
≤ 0.01 µs en todas las corridas, poco informativo.

| Escenario | Jitter prom (µs) | Jitter p99 (µs) | Jitter máx (µs) | Overruns por corrida (de 10 000) |
|---|---|---|---|---|
| Reposo | 87.0 | 588.5 | 18 908.5 | 93 (29 / 142 / 108) |
| CPU saturada (2 burners a prio 9, 93–99 % c/u) | 68.5 | 713.0 | 10 222.8 | 158 (248 / 117 / 110) |
| Control: lazo a prio 5, por debajo de la carga | 1 230.4 | 999.8 | **6 000 852.7** | 12 (1 corrida) |

Piso de ruido de la VM (`vm_noise`, hilo a prio 200 sin bloquearse, 10 s; no simultáneo con el lazo):

| Corrida | Hueco máx (µs) | Tiempo perdido | Huecos > 1 ms |
|---|---|---|---|
| Reposo (3) | 2 028.8 / 10 524.5 / 1 584.9 | 3.8 / 5.0 / 4.4 % | 3 / 15 / 1 |
| 1 burner en la otra vCPU (3) | 5 329.4 / 17 785.6 / 11 267.7 | 5.2 / 49.0 / 16.9 % | 13 / 504 / 130 |

### Interpretación
- **La prioridad aísla el lazo de la carga.** Con la misma carga, a prio 5 el lazo se queda bloqueado
  6 s completos (todo lo que dura la carga); a prio 50, su peor caso es 10 ms. El control también
  demuestra que la carga compite de verdad.
- **Saturar la CPU no degrada el lazo de forma distinguible.** La diferencia con el reposo (p99 713
  vs 588, overruns 158 vs 93) es menor que la variación entre corridas del mismo escenario.
- **El límite actual lo pone la VM.** Hilos que QNX no puede desplazar sufren huecos del mismo orden
  (1–18 ms) que los peores periodos del lazo. La correlación es por magnitud: no se midieron a la vez.
- **No se cumple un 1 kHz estricto en esta VM:** ~1–2 % de los ciclos son overruns, con picos de hasta
  19 ms. Estas cifras son una cota superior del sistema virtualizado, y no prueban cuánto
  mejoraría en hardware real, porque no se ha medido.
- El tiempo perdido incluye el servicio de interrupciones, no solo la vCPU retirada por el host.

### Hipótesis descartadas o sin resolver
- *El jitter en reposo se debe a despertar la vCPU de `HLT`:* no se sostiene. Hay huecos también en un
  hilo que nunca duerme, y la carga no mejora el resultado de forma consistente.
- *Anclar a una vCPU ayuda:* no concluyente. Con carga dio overruns 85 / 101 / 516 frente a 248 / 117 / 110
  sin anclar.
- *Periodo medio > 1000 µs (hasta 1 092):* parece pérdida de ticks bajo virtualización; causa no confirmada.

### Decisiones de diseño
- Prioridades: lazo de control 50, carga basura 9 (por debajo del shell y sshd, que corren a 10).
- El lazo no se ancla a una CPU por ahora; se reevaluará en M3 con datos.
- El tick queda en 1 ms: `ClockPeriod` no permite cambiarlo en ejecución en QNX 8.0.
- Se versionan los CSV crudos (5.4 MB) para que los resultados sean auditables.

### Problemas
Registrados en `CLAUDE.md` → "Errores conocidos y lecciones".

### Consecuencias para los siguientes hitos
- `breath_ctrl` no debe asumir que cada ciclo dura 1 ms: integrar con el tiempo medido y detectar
  ciclos perdidos.
- La latencia de las alarmas en esta VM tendrá la misma cola (decenas de ms en el peor caso). No se
  puede presentar como "garantizada" una cifra medida aquí sin esta aclaración.

## M2 — Pulse como paciente virtual (completado)

**Resultado: Pulse 4.4.0 corre dentro de QNX 8.0, sin cambios en su código. No hace falta el plan B por red.**

### Qué se hizo
- Pulse 4.4.0 compilado en Ubuntu, con datos generados por `DataSetReader` (Java). Python no fue necesario.
- `tools/pulse_probe/`: paciente sedado con el ventilador interno de Pulse o con presión externa por paso.
- `port/`: toolchain y receta para QNX (`build_pulse_qnx.sh`, `deploy_pulse_qnx.sh`).
- `scripts/m2_rc_check.py`: prueba el acople con el modelo RC. `scripts/m2_compare.py`: compara host contra QNX.
- `tareas/01_watchdog/`: primera tarea del modo tutor.
- Datos en `results/m2/`.

### API de Pulse que usaremos
- `CreatePulseEngine()` y `SerializeFromFile(estado)`: crea el motor y carga un paciente ya estabilizado.
- `SEDyspnea` (severidad 1.0): elimina la respiración propia, para simular a un paciente sedado.
- `SEMechanicalVentilation` + `ProcessAction`: aplica presión en la vía aérea (relativa a la atmósfera) y la mezcla de gases.
- `AdvanceModelTime()`: avanza un paso de 20 ms.
- `GetRespiratorySystem()`, `GetBloodChemistrySystem()`, `GetCardiovascularSystem()`: volumen, flujo, R, C, EtCO2, SpO2, FC.

### Resultados

Paso de tiempo de Pulse: **20 ms (50 Hz)**. Límite de tiempo real: 20 ms por paso.

| Entorno | Prom (ms) | p99 (ms) | Máx (ms) | Tiempo real | Pasos > 20 ms |
|---|---|---|---|---|---|
| Host, modo externo 20/5/12 | 1.40 | 2.64 | 5.1 | 14.2x | 0 de 9000 |
| QNX VM, reposo, 3 corridas | 2.20–2.23 | 3.7–3.9 | 26.4–47.2 | 9.0–9.1x | 2, 2, 1 de 9000 |
| QNX VM, CPU saturada, 3 corridas | 2.96–3.01 | 6.4–6.6 | 47.5–65.7 | 6.7–6.8x | 4, 1, 2 de 9000 |

Otros ajustes en host (30/10/20 y 12/0/10): máx 15–17 ms, 0 pasos > 20 ms. En la VM: máx 46–49 ms, 1–2 pasos > 20 ms.
Una corrida de 20 s justo después de recompilar fue peor (prom 3.8 ms, máx 75.8 ms, 10 de 1000): sin causa confirmada.

Fisiología del paciente (último tramo de cada corrida):

| Ajustes (Pinsp/PEEP/rpm, modo) | Vt (mL) | C (mL/cmH2O) | SpO2 | EtCO2 (mmHg) | FC |
|---|---|---|---|---|---|
| 20/5/12, ventilador interno | 579 | 58.0 | 97.2 % | 39.9 | 72 |
| 20/5/12, presión externa | 588 | 58.0 | 98.1 % | 32.5 | 72 |
| 30/10/20, presión externa | 607 | 68.4 | 98.9 % | 23.1 | 71 |
| 12/0/10, presión externa | 428 | 47.4 | 93.8 % | 41.9 | 80 |

Host contra QNX: **todas las columnas fisiológicas coinciden en todos los pasos**, y también reposo contra CPU saturada.
Es a la precisión del CSV (0.01 mL, 0.0001 de SpO2), no una comparación bit a bit.

Acople con el modelo RC (error de volumen, % del Vt):

| Ajustes | R y C fijos (libre) | Resincronizando cada 20 ms |
|---|---|---|
| 20/5/12, tiempo no visto | RMS 1.0 %, máx 2.0 % | RMS 0.01 %, máx 0.02 % |
| 30/10/20 | RMS 13.4 %, máx 16.2 % | RMS 0.01 %, máx 0.02 % |
| 12/0/10 | RMS 4.3 %, máx 7.4 % | RMS 0.01 %, máx 0.05 % |

### Interpretación
- **Hay margen de CPU, pero hay picos.** En promedio Pulse usa ~11–15 % del presupuesto en la VM. Aun así, ~0.02–0.04 % de los pasos pasan de 20 ms, con picos de hasta 66 ms.
- **`breath_ctrl` no puede depender de Pulse en sus 1 ms.** Debe correr por separado y tolerar que Pulse se retrase.
- **La simulación es reproducible:** el tiempo real no entra en las ecuaciones. En estas corridas sin lazo de control, un retraso no cambia la fisiología.
- **El error de acople solo se mide en los puntos de 20 ms.** Pulse no da datos entre medias, y la presión se mantuvo constante en cada paso.
- **EtCO2 aún no se estabiliza a los 180 s** y baja con presión externa. Hay diferencia con el ventilador interno (32.5 vs 39.9 mmHg, a Vt parecido); la causa probable es el espacio muerto del circuito, sin confirmar.
- **Estos pasos no se ejecutaron con ritmo de tiempo real ni con prioridad alta** (Pulse a prio 10, con escritura de CSV entre pasos). Falta medirlo en M3.

### Decisiones de diseño
- Pulse corre en QNX como proceso `patient` a 50 Hz, con prioridad por debajo de `breath_ctrl` y `safety_mon`.
- Acople: `pneumatic_sim` integra un RC a 1 kHz, envía a Pulse la presión promedio de cada 20 ms y se resincroniza con su volumen, flujo, R y C.
- Protobuf 21.12 para QNX: sin dependencia de absl. El host conserva protobuf 35; cada build genera sus propios bindings.
- Pulse se compila fuera del repo (`~/pulse`); en el repo solo va la receta. Se usa `libc++` por ser la biblioteca de C++ de QNX.
- La VM aguanta: binario de 13 MB, datos ~3 MB, libres 24.9 MB en `/data` tras la campaña. Lectura de memoria: unos 21 MB usados a los 8 s de arranque; no se midió el pico.

### Pendientes y límites
- `build_pulse_qnx.sh` se ejecutó completa sobre un build existente, no desde cero en una máquina limpia.
- Los valores de R y C son los que reporta Pulse; no se contrastaron con datos clínicos.

### Problemas
Registrados en `CLAUDE.md` → "Errores conocidos y lecciones".

### Siguiente paso
M3: `pneumatic_sim` + `breath_ctrl`, pendiente de revisión de M2 por el usuario.
