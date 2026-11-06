# BreathGuard RT — Progreso

Entorno de todas las mediciones: QNX SDP 8.0, QEMU x86_64 con KVM (2 vCPU, 1 GB) sobre
WSL2/Windows, toolchain `qcc -Vgcc_ntox86_64`. Tick del sistema: 1 ms.

Cambio de enfoque (2026-11-06): el proyecto pasó de MedPower (continuidad eléctrica) a BreathGuard RT
(controlador de ventilador con paciente virtual). M1 se conserva porque no depende del producto.

| Hito | Contenido | Estado |
|---|---|---|
| M1 | Prueba de latencia | Completado |
| M2 | Pulse en el host, API y paso de tiempo, evaluación del port a QNX | En curso |
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
