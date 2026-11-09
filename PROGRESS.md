# BreathGuard RT — Progreso

Entorno de todas las mediciones: QNX SDP 8.0, QEMU x86_64 con KVM (2 vCPU, 1 GB) sobre
WSL2/Windows, toolchain `qcc -Vgcc_ntox86_64`. Tick del sistema: 1 ms.

Cambio de enfoque (2026-11-06): el proyecto pasó de MedPower (continuidad eléctrica) a BreathGuard RT
(controlador de ventilador con paciente virtual). M1 se conserva porque no depende del producto.

| Hito | Contenido | Estado |
|---|---|---|
| M1 | Prueba de latencia | Completado |
| M2 | Pulse en el host, API y paso de tiempo, port a QNX | Completado |
| M3 | `pneumatic_sim` + `breath_ctrl` (PCV, VCV) | Completado (falta la tarea 2 del usuario) |
| M4 | `safety_mon` | En curso (escenas en la VM hechas; falta la tarea 1 del usuario) |
| M5 | `power_mgr` | Completado |
| M6 | HMI, dashboard, comandos del operador, versión monolítica | Completado (contraste medido; tarea 3 del usuario pendiente) |
| M7 | Entrega | En curso (demo grabable lista; falta el video) |

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
- **Hay margen de CPU, pero hay picos.** En promedio Pulse usa ~11–15 % del presupuesto en la VM. Aun así, ~0.01–0.04 % de los pasos pasan de 20 ms, con picos de hasta 66 ms.
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
M3: `pneumatic_sim` + `breath_ctrl`.

## M3 — Control de la respiración (en curso)

### Paso 2: Pulse como proceso `patient` (prio 20), acoplado a la planta
- `src/patient.cpp`: carga el estado, aplica `SEDyspnea` y `SEMechanicalVentilation`, y pide ventanas con `MsgSend`.
- `pneumatic_sim`: corta su tiempo en ventanas de 20 ms (Paw promedio por integral exacta, volumen al cierre). Si la
  ventana pedida aún no se cerró, retiene la respuesta (respuesta diferida) hasta cerrarla. Con cada resultado:
  `dV = V_Pulse(k) − V_planta(k)` se suma al volumen actual; R, C y V0 = V − C·(Paw_k − Q·R) se actualizan.
  Anillo de 256 ventanas (5.12 s) para tolerar atrasos.
- **R y C de Pulse son promedios por respiración que tardan varias en converger** (R 1.5 → 22). Arrancando con el
  paciente nunca ventilado, las correcciones llegaron a 39 mL/ventana. Solución: `patient -W 120` guarda una vez
  `BreathGuard_StandardMale_PCV.json` (120 s de PCV 20/5/12, 15.6 s en la VM; R 22.9, C 56.9).
- **Error corregido:** con `patient` atrasado, las ventanas ya cerradas guardaban el volumen sin la corrección
  anterior y se contaba dos veces (|dV| hasta 1.7 mL). Ahora se desplazan también.
- Prueba de humo, 20 s: 1050 ventanas, 0 perdidas, paso máx de Pulse 95.8 ms (atraso 3 ventanas);
  |dV| p50 0.028, p99 0.097, máx 0.103 mL (≤ 0.02 % del Vt). Vt 569 mL; SpO2 98.0 %, EtCO2 34.1, FC 72.
- `/tmp` de la VM está en `/data` (62 MB); `m3_run.sh` borra los CSV tras traerlos.

### Medición con Pulse: PCV 20/5, 12 rpm, FiO2 21 %, 120 s por corrida
Planta prio 60, control 50, Pulse 20. Carga: 2 hilos a prio 9. Control: Pulse a prio 5, por debajo de la carga.
`scripts/m3_pulse_summary.py` → `results/m3/pulse_summary.txt`. En el repo van los CSV de Pulse de todas las corridas y
los de planta y control de reposo 1, carga 1 y control; el resto se guardó fuera del repo (`~/medpower_data/m3`).

| Corrida | Ciclos | Tardíos | Pasos Pulse > 20 ms | Paso máx | Atraso máx (ventanas) | Perdidas | \|dV\| p99 / máx (mL) | Vt | SpO2 | EtCO2 | FC |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Reposo 1 | 117 494 | 1 799 | 63 | 134 ms | 12 | 0 | 0.098 / 0.107 | 569 | 98.0 | 33.6 | 72 |
| Reposo 2 | 118 901 | 1 021 | 29 | 74 ms | 3 | 0 | 0.096 / 0.104 | 569 | 98.0 | 33.6 | 72 |
| Reposo 3 | 118 119 | 1 477 | 51 | 114 ms | 12 | 0 | 0.098 / 0.111 | 569 | 98.0 | 33.6 | 72 |
| Carga 1 | 115 652 | 3 163 | 59 | **1 350 ms** | 66 | 0 | 0.102 / 0.292 | 569 | 98.0 | 33.6 | 72 |
| Carga 2 | 103 263 | 8 338 | 382 | 553 ms | 40 | 0 | 0.128 / 0.206 | 568 | 98.0 | 33.7 | 72 |
| Carga 3 | 115 036 | 2 924 | 95 | 137 ms | 18 | 0 | 0.097 / 0.112 | 569 | 98.0 | 33.6 | 72 |
| Control (Pulse prio 5) | 116 880 | 2 504 | — | — | — | — | 0.103 (antes de la carga) | 562 | congelado | congelado | — |

### Interpretación
- **El acople aguanta atrasos de Pulse de hasta 1.35 s sin perder ventanas** (anillo de 5.12 s) y con correcciones
  ≤ 0.3 mL (0.05 % del Vt). La fisiología final es la misma en reposo y con carga, porque Pulse es determinista y la
  presión que recibe apenas cambia.
- **Control:** con Pulse por debajo de la carga, `patient` solo simuló las 101 ventanas previas a la carga y luego no
  recibió CPU en todo el minuto y medio siguiente. **La ventilación siguió** (Vt 562 mL, con el último R y C), pero los
  signos vitales quedaron congelados. Las prioridades protegen lo crítico; lo no crítico se degrada. Lección para la
  HMI: mostrar la antigüedad de los signos vitales.
- Los pasos lentos de Pulse empeoran con carga (máx 0.55–1.35 s), aunque Pulse está por encima de ella: el host
  retira vCPU (ver M1, M3 paso 1). Carga 2 tuvo además 66 % de CPU en los burners y 8 338 ciclos tardíos: corrida ruidosa.
- Vt 569 mL frente a 588 de M2 (presión cuadrada ideal): rampa de 100 ms y resistencia de la rama espiratoria.
- EtCO2 33.6 mmHg (rango normal 35–45): el paciente está algo hiperventilado con 20/5 y 12 rpm. SpO2 98 % con aire ambiente.

### Problemas
- Ver `CLAUDE.md`: `msg_t` ya existe en QNX; terminar los lazos por tiempo y no por número de ciclos.

### Paso 1: planta + PCV como procesos QNX (paciente RC fijo)
- `src/plant.c`: soplador (1.er orden, 40 ms), rama inspiratoria con antirretorno, tubuladura (1.5 mL/cmH2O),
  válvula espiratoria + PEEP, pulmón RC (R 22.4, C 58 mL/cmH2O), ruido de sensores y fallos inyectables.
  Subpasos de 50 µs.
- `src/pcv.c`: PCV con rampa de 100 ms, PI (kp 2, ki 15) con prealimentación y antisaturación; tiempo medido.
- `src/pneumatic_sim.c` (prio 60): `name_attach`, timer de 1 ms, mensajes READ/WRITE/FAULT; integra hasta
  "ahora" antes de cada mensaje.
- `src/breath_ctrl.c` (prio 50): `name_open`, timer de 1 ms, READ → PI → WRITE con `MsgSend`.
- `tools/m3_offline.c`: el mismo código en Ubuntu, sin QNX. `scripts/m3_run.sh` / `m3_analyze.py`.

Offline (Ubuntu), 30 s: Vt 576 / 510 / 516 mL con 20/5/12, 30/10/20, 12/0/10; entre 2 y 6 % bajo la fórmula RC con
presión cuadrada (rampa de 100 ms y resistencia espiratoria). Sobrepico de 0.46 cmH2O; PEEP ±0.25 cmH2O.

Prueba de humo en QNX (6 s): los procesos se comunican (6000 READ, 6000 WRITE) y la forma de onda coincide con la
offline.

### Medición en QNX: PCV 20/5 cmH2O, 12 rpm, I:E 1:2, 30 s por corrida, 3 en reposo y 3 con carga
Carga: `cpu_burn`, 2 hilos FIFO a prio 9 (86–100 % de CPU cada uno), lanzada 1 s después del lazo.
`breath_ctrl` termina por tiempo de reloj (ver "Problemas"). Datos en `results/m3/` (CSV comprimidos),
`late_summary.txt` con `scripts/m3_late.py`.

| Corrida | Ciclos en 30 s | Periodo > 1.5 ms | Periodo máx | Ticks de la planta > 1.5 ms | Vt resp. 1–5 (mL) | PIP máx |
|---|---|---|---|---|---|---|
| Reposo 1 | 29 933 | 95 | 6.2 ms | 21 | 577, 576, 576, 576, 576 | 20.5 |
| Reposo 2 | 29 743 | 85 | 26.0 ms | 36 | 577, 576 ×4 | 20.5 |
| Reposo 3 | 29 946 | 56 | 12.6 ms | 20 | 577, 576 ×4 | 20.5 |
| Carga 1 | 24 632 | 2 464 | 22.0 ms | 2 355 | 577, 576 ×4 | 20.5 |
| Carga 2 | 26 728 | 1 911 | 14.6 ms | 1 811 | 578, 576 ×4 | 20.5 |
| Carga 3 | 26 788 | 1 928 | 27.5 ms | 617 | 577, 577, 576 ×3 | 20.5 |

Desvío de Paw real contra el objetivo en la meseta: p99 normal 0.25–0.27 cmH2O; peor dentro de un hueco del
controlador 0.15–0.33 cmH2O (hueco de hasta 11.2 ms). Ubuntu (offline): Vt 576 mL, PIP 20.5.
Cómputo por ciclo (READ + PI + WRITE, tiempo de pared): p50 9–54 µs, p99 95–227 µs; máx hasta 11 ms con carga
(el hilo fue interrumpido dentro del ciclo).

### Interpretación
- **La ventilación no cambió con la carga ni con los ciclos tardíos:** Vt idéntico a Ubuntu (±2 mL) en las 6 corridas.
  Razones: la planta integra el tiempo real; un ciclo tardío deja el último comando, y el soplador (40 ms) y el
  pulmón (τ 1.3 s) son lentos frente a huecos de ≤ 27 ms; el PI usa el dt medido.
- **Con carga se pierde el 7–18 % de los ticks, también en la planta (prio 60)**, que está muy por encima de la carga
  (prio 9). Lo más probable es que sea el host retirando las vCPU cuando el invitado las satura (M1, `vm_noise`: hasta
  49 % de tiempo perdido con un burner). No se midió `vm_noise` en simultáneo: es la explicación más probable, no una prueba.
- **No es tiempo real estricto en esta VM:** 0.2–0.3 % de ciclos tardíos en reposo y 7–10 % con carga. Que la
  respiración no cambie demuestra tolerancia del diseño, no cumplimiento del 1 kHz.
- La planta comparte la VM con el controlador: si el host congela la VM, se congelan los dos, y la planta integra el
  hueco al reanudar. Es equivalente a un controlador que llega tarde, pero un hueco de la planta no es realista para
  hardware físico.

### Paso 3: VCV (offline)
`src/vent.c` reemplaza a `pcv.c`: PCV igual que antes, y VCV con flujo constante (PI de flujo, kq 20, ki 50, subida
de 100 ms) durante el 80 % de Ti más una pausa inspiratoria, y límite de presión de 40 cmH2O que corta la inspiración.
Offline: Vt dentro de ±3.2 % con pulmón normal, rígido (C 30) y obstruido (R 40); con C 15 y 500 mL, el límite corta
y entran 392 mL. Pendiente: correr VCV en la VM con Pulse.

## M4 — Monitor de seguridad (en curso)

### Qué se hizo
- `src/safety_mon.c` (prio 55): lanza `breath_ctrl` con `posix_spawn`; lo vigila por muerte
  (`_PULSE_CODE_DISCONNECT` de su conexión), por cuelgue (watchdog de latidos de la tarea 1, plazo 200 ms) y por
  apnea con el controlador vivo. Ante una falla: `BG_MSG_OVERRIDE` a la planta, ventilación de respaldo (mismo
  `vent.c`, en fase gracias a `-T`), relanzamiento y devolución del mando al empezar una respiración tras 50 latidos.
- `src/alarms.c`: presión alta (40 cmH2O, 3 ms; con alivio de presión), desconexión (20 ms), apnea (12 s). Vt bajo,
  Vt alto y fuga con `vt_meter` (tarea 2). `tools/m4_offline.c`: sin falsas alarmas en 7 escenarios de 120 s.
- `breath_ctrl`: latidos (`MsgSendPulse` cada 10 ciclos), despedida (`BG_PULSE_BYE`) y `SIGTERM` limpio.
- `pneumatic_sim`: arbitraje de actuadores (override) y registro de quién manda; registra las fallas inyectadas.
- `src/bg_fault.c`: inyección de fallas. `scripts/m4_run.sh` / `m4_target.sh` / `m4_analyze.py`: escenas.

### Escenas en la VM (PCV 20/5, 12 rpm, con Pulse; falla a los 40 s)
| Escena | Detección | Respaldo manda tras el último comando | Vt en las respiraciones afectadas | SpO2 mín |
|---|---|---|---|---|
| Normal (60 s) | sin eventos ni falsas alarmas | — | 568–569 | 97.94 % |
| Matar `breath_ctrl` (`slay -s KILL`) | CONTROL_MUERTO, 1.6 ms | 2.4 ms; mando devuelto a los 5.5 s | 569, 568, 569 | 97.94 % (sin cambio) |
| Matar con CPU saturada | CONTROL_MUERTO, 1.3 ms | 2.6 ms; devuelto a los 5.5 s | 569, 568, 568 | 97.94 % (sin cambio) |
| Congelar (`SIGSTOP`), watchdog sin completar | APNEA, 9.2 s | 9.2 s | **0, 0**, luego 607 | **97.04 %** |
| Desconectar el tubo 15 s | DESCONEXION, 20.6 ms; APNEA a los 7.5 s (solo alarma) | — (no es falla del control) | **0, 0, 0** | **95.6 %**, se recupera |

| Obstrucción R x8 en VCV 500 mL (15 s) | PRESION_ALTA en cada respiración (38 cmH2O; 3 ms tras el cruce) | — (el controlador limita a 40) | **148–157** (504 normal) | **96.5 %**, se recupera |

VCV en la VM con Pulse: Vt 503–504 mL para 500 pedidos.

### Repeticiones (tras corregir el orden en safety_mon; `results/m4/repeticiones.txt`)
Marca de tiempo con `bg_fault -n` justo antes de `slay`.
| Escena | Último comando → respaldo manda | `slay` → respaldo manda | Detección → override | SpO2 |
|---|---|---|---|---|
| Matar ×3 | 1.9 / 2.0 / 2.0 ms | 3.8 / 6.9 / 7.1 ms | 0.02–0.23 ms | sin cambio |
| Matar con CPU saturada ×3 | **24.3** / 2.0 / 3.0 ms | **53.9** / 6.6 / 6.9 ms | 0.04–0.11 ms | sin cambio |
| Desconectar ×3 | alarma a 20.6 / 20.7 / 20.4 ms | — | — | 95.6 % (×3) |

- `slay` tarda 2–5 ms en matar al proceso (lo busca por nombre). Una corrida con carga tuvo una cola de 24 ms: la cola
  de la VM de M1. Lanzar el nuevo controlador (`posix_spawn`) tarda 0.7–1.5 ms y ya ocurre después de tomar el mando.
- **Error corregido:** la primera versión hacía `printf` y `posix_spawn` antes de mandar el override; en una corrida
  eso sumó 30 ms. Ahora el override sale 0.02–0.23 ms tras la detección y los eventos se imprimen al final.

### Errores encontrados por las escenas
- **Sin alarma de presión alta cuando el controlador limita:** el controlador corta en cuanto Paw > 40 y la alarma
  pedía 40 sostenidos 3 ms → ahora la alarma es a 38 cmH2O y el alivio de `safety_mon` (por si el controlador no
  limita) a 40.
- **La apnea mataba a un controlador sano durante una desconexión** → ahora la apnea solo activa el respaldo si no
  hay alarma de desconexión.
- **Pulse no se enteraba de la desconexión:** recibía la presión del circuito y su pulmón seguía inflándose → ahora la
  planta le manda la presión en la boca del paciente (0 si el tubo está suelto).

### Interpretación
- **La muerte del controlador no afecta al paciente**, ni con la CPU saturada: el kernel avisa en ~1.5 ms y el
  respaldo manda en ~2.5 ms, frente a respiraciones de 5 s y un soplador de 40 ms.
- **El cuelgue sin watchdog cuesta 2 respiraciones** y 1 punto de SpO2: es exactamente lo que cubre la tarea 1.
- **Una desconexión sí afecta al paciente** (SpO2 98 → 95.6 % en 15 s): la arquitectura no puede reconectar un tubo;
  su trabajo es avisar en ~20 ms.
- Pendiente: repetir las escenas; escena del alivio de presión con un controlador que no limita.

## M5 — Gestión de energía (completado)

### Qué se hizo
- `src/battery.c`: batería de ion-litio 7S2P (6 Ah, 24 V) con circuito de Thevenin de primer orden (R0 0.12 Ω,
  R1 0.08 Ω, C1 600 F) y curva Voc(SoC); carga a 1.5 A con red. Consumos: base 6 W, soplador 8 W en vacío + potencia
  neumática / 0.25, pantalla 7 W (2 W atenuada), humidificador 35 W, telemetría 2 W.
- `src/power_mgr.c` (prio 30): muestrea la red cada 1 ms (corte = < 18 V durante 2 muestras), lee la planta cada
  10 ms para la potencia del soplador, avanza la batería (con escala de tiempo `-x` para la demo), recorta lo no
  esencial (humidificador apagado, pantalla atenuada; la telemetría sigue), estima la autonomía (energía restante /
  potencia media) y avisa a `safety_mon` con un pulso (`BG_PULSE_POWER`).
- `bg_fault -m 0|1`: corta o restablece la red. `scripts/m5_run.sh`, `m5_target.sh`. `tools/m5_offline.c`.

### Resultados
Offline (PCV 20/5): 58.9 W con todo, 18.9 W recortando. Autonomía desde el 100 %: 2.59 h y 8.24 h; el estimador
acierta dentro de +3 % y +1 %.

En la VM, con el sistema completo y Pulse:
| Escena | Resultado |
|---|---|
| Corte de red a los 30 s, vuelve a los 75 s (×60) | corte detectado a 1.8–2.0 ms; autonomía 160 → 524 min al recortar; SoC 100 → 92 % en 45 min de batería; recarga a 1.5 A; Vt 567–569 mL y SpO2 98.0 % sin cambio |
| Batería al 25 %, corte a los 5 s (×120) | BATERÍA BAJA al 20 % (44 min de batería), BATERÍA CRÍTICA al 5 % (112 min); estimado al cortar: 141 min, real hasta el 5 % + 21 min estimados = 133 min (−6 %) |

### Errores corregidos
- Tras el corte, la autonomía usaba ~10 s el consumo de antes → el promedio se reinicia al cambiar de modo.
- Con el promedio reiniciado la autonomía valía 0 y disparaba BATERÍA BAJA falsa → se usa la potencia instantánea.

### Límites
- Los parámetros de batería y consumos son típicos, no medidos en un equipo real.
- El ventilador simulado no se apaga al agotarse la batería: `power_mgr` no corta la planta.
- La escala de tiempo acelera solo la batería; la ventilación y las alarmas siguen en tiempo real.

## M6 — HMI, dashboard, comandos del operador y contraste monolítico (completado)

### Qué se hizo
- `src/hmi.c` (prio 10): cada 10 ms lee la planta y cada 100 ms los signos vitales (`BG_MSG_VITALS`), el estado de
  `safety_mon` (`BG_MSG_SAFETY_STATUS`, ahora con los ajustes vigentes) y el de `power_mgr`; manda una línea CSV por UDP
  al host (172.31.1.1:5005). Bugs de demo: SIGUSR1 (puntero nulo), SIGUSR2 (bucle infinito).
- `safety_mon -H ip`: lanza la HMI, la relanza si muere (`_PULSE_CODE_DISCONNECT`) y la mata si deja de consultar
  2 s (cuelgue).
- **Dashboard** `scripts/dashboard.py`: curvas de Paw, flujo y volumen (15 s), SpO2/EtCO2/FC, Vt, Ppico/PEEP, quién
  controla, alarmas, energía, ajustes y "NO DATA" si la HMI calla. Modos: en vivo (UDP), `--replay`, `--snapshot`.
  Vt y PEEP comprobados contra la planta (572 vs 569 mL; 5.1 vs 5.1 cmH2O; VCV 455 vs 454 mL).
- **Comandos del operador**: `scripts/bg_cmd.py p_insp=25 peep=8 rr=15 ie=2 vt=450 mode=pcv|vcv` → UDP 5006 a la HMI
  → `BG_MSG_SETTINGS_SET` a `safety_mon`, que valida (`src/settings.c`) y programa el cambio para el inicio de la
  próxima respiración (`t0` común). `breath_ctrl` consulta cada 100 ms (`BG_MSG_SETTINGS_GET`) con `TimerTimeout` de
  2 ms y aplica en `t0`; el respaldo cambia en el mismo instante; el relanzado arranca con los ajustes vigentes.
- **Versión monolítica** `src/mono_vent.c`: control + pantalla en un solo lazo, sin supervisor, con los mismos bugs.
- Tarea 3 del usuario: `tareas/03_rate_limit` (límite de cambio por comando; el esqueleto acepta todo).

### Escenas de la HMI en la VM (con Pulse; señal a los 30 s; 60 s)
| Escena | Qué hace safety_mon | Hueco en la telemetría | Ventilación |
|---|---|---|---|
| Matar la HMI (KILL) | relanza a los ~0.1 s | < 0.1 s | Vt 568–569, SpO2 98 % sin cambio |
| Bug: puntero nulo (SIGUSR1) | relanza a los 0.1 s | 99 ms | sin cambio |
| Bug: bucle infinito (SIGUSR2) | HMI_COLGADA a los 2.0 s, la mata y relanza | 1996 ms | sin cambio |

### Contraste: el mismo bug de pantalla en dos arquitecturas (90 s, bug a los 31 s; `results/m6/contrast.*`)
| Escena | Edad máx. del último comando (25 s tras el bug) | SpO2 mín | FC máx |
|---|---|---|---|
| BreathGuard, puntero nulo | 0.01 s | 97.9 % | 72 |
| BreathGuard, bucle infinito | 0.00 s | 97.9 % | 72 |
| Monolito, puntero nulo | 24.98 s (no vuelve a mandar) | 90.6 % | 86 |
| Monolito, bucle infinito | 24.98 s | 89.6 % | 90 |

En el monolito la planta queda con el último comando (soplador 16–19 %, válvula espiratoria cerrada): pulmón inflado a
~10–11 cmH2O sin ventilar. Ambas corridas cayeron al inicio de una inspiración (el bug llega 30 s después de arrancar
el control, múltiplo del periodo de 5 s).

### Escena de ajustes (90 s; `results/m6/ajustes_respiraciones.txt`, `cmd_ajustes.txt`)
- p_insp 20→25: ACK a los 16.9 s; aplicado en la respiración de 20.5 s (Ppico 25.6, Vt 569→767 mL).
- p_insp=50 → NAK "fuera de 5-35 cmH2O"; `foo=3` → NAK "clave desconocida".
- 25/8 a 15 rpm: aplicado en 35.5 s; periodo 5→4 s, PEEP 8.2.
- `kill -9` a breath_ctrl a los 55 s: respaldo con 25/8 a 15 rpm sin salto; relanzado con los mismos ajustes; mando
  devuelto en 59.5 s.
- VCV 450 mL: aplicado en 63.5 s; Vt 454 mL. breath_ctrl: 0 consultas sin respuesta.

### Errores corregidos
- Una señal con manejador interrumpe `MsgReceivePulse` con `EINTR`; se trataba como fatal. Corregido en `hmi` y
  `breath_ctrl`.
- `MsgSend` devuelve el estado de `MsgReply` (0), no los bytes: todas las consultas de ajustes parecían fallar.
- Detector de respiraciones del dashboard sin histéresis correcta; PEEP mal definida (mínimo con ruido).

### Límites
- Cambios de ajustes solo al inicio de una respiración; no hay confirmación de cambios grandes (tarea 3: límite por
  comando).
- El monolito no tiene watchdog de hardware; con uno se reiniciaría en segundos (respiraciones perdidas igualmente).

## M7 — Entrega (en curso)

### Demo grabable
- `scripts/demo_run.sh` (+ `demo_target.sh`): el sistema completo 180 s en la VM; el host dispara las escenas por SSH y
  manda subtítulos al dashboard (`CAPTION,texto` por UDP). El dashboard graba lo recibido (`--record`).
- Telemetría nueva: la planta reporta la antigüedad máxima del último comando aplicado (de breath_ctrl o del
  respaldo) cada 100 ms; el dashboard la muestra como "Loop 1 kHz: command age max".
- Resultados de la corrida de referencia (`results/demo/*_20261109_091447*`, detalle en `docs/demo.md`): HMI relanzada
  sin cambio en el Vt; con la CPU saturada Vt 569 en todas las respiraciones y antigüedad del comando ≤ 13 ms (mediana
  1.3 ms); desconexión detectada a 21.1 ms (SpO2 mín 96.3 %); respaldo 0.18 ms tras detectar la muerte del control;
  ajuste aplicado en la respiración siguiente; corte de red detectado en 1.9 ms. SpO2 final 98.5 %.

### Bug encontrado por la demo: pico de presión al reconectar el tubo
- Corrida `20261109_090520`: al reconectar en plena inspiración, Paw 44.3 cmH2O en 3 ms; saltó el alivio de
  `safety_mon` (40). Causa: con el tubo abierto el PI de PCV llevaba el soplador a 60 cmH2O.
- Arreglo (`src/vent.c`, `PCV_HEADROOM` 8 cmH2O): el soplador de PCV no pasa de p_insp + 8; el anti-windup usa ese tope.
  Offline: 44.1 → 23.5 cmH2O; Vt normal igual (576 mL); solo pulmones irreales (C 100, R 5) pierden hasta 3 % de Vt.
  En QNX: 23.3 cmH2O, sin alarma.


### Revisión del dashboard (2026-11-09): métricas honestas, ventilación protectora, auto-PEEP, paquete MATLAB
- **Métricas de tiempo real en vivo.**
  - `breath_ctrl` lleva histogramas del periodo (clases de 10 µs) y del cómputo (1 µs). Cada 100 ms manda p50, p99,
    p99.9, máximo, plazos incumplidos (> 1.5 ms), periodos perdidos y un histograma log de 28 clases
    (`bg_loop_stats_t`), dentro de la consulta de ajustes que ya existía (acotada con `TimerTimeout`).
  - `pneumatic_sim` lleva el histograma de la antigüedad del comando desde el primer comando (p99 y máximo).
  - La HMI manda una línea `RT,...` a 1 Hz.
  - El dashboard tiene un panel con colores por umbral (verde ≤ 1.5 ms, ámbar ≤ 20 ms, rojo), un histograma
    log-log y la nota de la VM.
- **Ventilación protectora.**
  - `safety_mon -h talla -g m|f` calcula el peso ideal (ARDSNet): 75.4 kg para StandardMale.
  - El límite de Vt alto es 8 mL/kg (603 mL) en PCV; en VCV, el menor entre +20 % y 8 mL/kg. Este límite depende de la
    tarea 2.
  - El dashboard muestra Vt en mL/kg con colores.
  - La escena del operador de la demo pasó de p_insp 25 (10.2 mL/kg) a PEEP 8 / p_insp 23 (misma presión de
    distensión, 7.6 mL/kg).
- **Auto-PEEP** (`src/lung_mon.c`, en `safety_mon`).
  - Ajuste exponencial del flujo espiratorio, R_circ medida y continuidad de la presión alveolar.
  - auto-PEEP = |Q_fin_esp| × R_total.
  - Alarma `AUTO_PEEP` (bit 9) si supera `-A` (3 cmH2O) en 2 respiraciones seguidas.
  - Prueba en el host contra la presión alveolar verdadera (`tools/lung_mon_test.c`): 7 casos, error ≤ 0.17 cmH2O.
- **Datos clínicos.**
  - FiO2 (Pulse → planta → HMI).
  - FR, I:E y ventilación minuto medidas (dashboard).
  - Límites de alarma activos (`safety_mon` → HMI).
- **Marcadores** verticales en las tres curvas: falla del controlador, HMI relanzada, desconexión y reconexión, corte
  y vuelta de red, ajustes aplicados.
- **Paquete para MATLAB** (`design/`).
  - `plant_model.md` y `controller.md`: ecuaciones, parámetros, discretización, saturaciones, anti-windup y fases.
  - CSV de PCV 20/5 12 rpm 60 s a 1 kHz (QNX y host con reloj ideal) y de escalones en lazo abierto (QNX con
    `src/step_test.c`, y host).
  - Se regeneran con `scripts/design_run.sh` (`tools/design_ref.c`, `scripts/design_export.py`).
  - QNX contra ideal: 0.09 cmH2O y 0.4 mL RMS; las diferencias grandes solo aparecen en el cambio de inspiración a
    espiración.

Resultados en QNX:
- Escena `autopeep` (`scripts/m6_run.sh ap 75 autopeep`, con Pulse):
  - a 30 rpm 1:1, estimado 5.1–5.4 cmH2O (verdad del modelo RC fijo: 5.08) y flujo final −12.3 L/min;
  - alarma 4 s después del cambio; se apaga en la primera respiración a 12 rpm;
  - Vt 293 mL (3.9 mL/kg);
  - R 23.9 y C 59.3 estimadas (Pulse ~22.4 y 58–63).
- Ajustes por defecto: auto-PEEP ~1.0–1.2 cmH2O (τ del paciente 1.4 s; espiración de 3.3 s). Es real; ahora se ve.
- Ruido de la VM ese día: 0.3–3 % de ciclos > 1.5 ms y máximos de 7–82 ms, con y sin carga (300 ticks tardíos en los
  primeros 10 s, sin carga).

Error corregido: la antigüedad del comando contaba desde el arranque de la planta, antes del primer comando (máximo
1000 ms, p99 200 ms). Ahora cuenta desde el primer WRITE u OVERRIDE.

### Pendiente
- Grabar el video (< 3 min) con `scripts/demo_run.sh` y el contraste monolítico; subirlo a YouTube.
- Repetir la escena "congelar" cuando el watchdog (tarea 1) funcione.
