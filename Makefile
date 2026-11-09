# BreathGuard RT - build para QNX SDP 8.0, target QEMU x86_64.
# Antes: source ~/qnx800/qnxsdp-env.sh

CC      := qcc
TARGET  := gcc_ntox86_64
CFLAGS  := -V$(TARGET) -Wall -Wextra -O2 -g

QNX_HOST_SSH := root@172.31.1.2
QNX_DIR      := /tmp/medpower

BUILD := build
BINS  := $(BUILD)/latency_test $(BUILD)/cpu_burn $(BUILD)/vm_noise

TAREA1 := tareas/01_watchdog
TAREA2 := tareas/02_vt_meter
TAREA3 := tareas/03_rate_limit
TAREA4 := tareas/04_plant_auth
TAREA5 := tareas/05_tree

# M3: procesos del ventilador (varios fuentes cada uno)
M3_BINS := $(BUILD)/pneumatic_sim $(BUILD)/breath_ctrl
M4_BINS := $(BUILD)/safety_mon $(BUILD)/bg_fault
M5_BINS := $(BUILD)/power_mgr
M6_BINS := $(BUILD)/hmi $(BUILD)/mono_vent
DESIGN_BINS := $(BUILD)/step_test
SEC_BINS := $(BUILD)/evil_hmi
VENTD_BINS := $(BUILD)/ventd
COMMON_H := src/bg_types.h src/bg_proto.h src/rt_util.h src/settings.h

.PHONY: all clean deploy check-env tarea1 tarea2 tarea3 tarea4 tarea5 tarea5-host copilot-logic-test m3 m4 m5 m6 patient

patient: check-env $(BUILD)/patient

tarea1: check-env | $(BUILD)
	$(CC) $(CFLAGS) -I$(TAREA1) $(TAREA1)/watchdog.c $(TAREA1)/test_watchdog.c -o $(BUILD)/test_watchdog
	ssh -o LogLevel=ERROR $(QNX_HOST_SSH) "mkdir -p $(QNX_DIR)"
	scp -o LogLevel=ERROR -q $(BUILD)/test_watchdog $(QNX_HOST_SSH):$(QNX_DIR)/
	ssh -o LogLevel=ERROR $(QNX_HOST_SSH) "$(QNX_DIR)/test_watchdog"

tarea2: check-env | $(BUILD)
	$(CC) $(CFLAGS) -I$(TAREA2) $(TAREA2)/vt_meter.c $(TAREA2)/test_vt_meter.c -lm -o $(BUILD)/test_vt_meter
	ssh -o LogLevel=ERROR $(QNX_HOST_SSH) "mkdir -p $(QNX_DIR)"
	scp -o LogLevel=ERROR -q $(BUILD)/test_vt_meter $(QNX_HOST_SSH):$(QNX_DIR)/
	ssh -o LogLevel=ERROR $(QNX_HOST_SSH) "$(QNX_DIR)/test_vt_meter"

tarea3: check-env | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $(TAREA3)/rate_limit.c $(TAREA3)/test_rate_limit.c -lm -o $(BUILD)/test_rate_limit
	ssh -o LogLevel=ERROR $(QNX_HOST_SSH) "mkdir -p $(QNX_DIR)"
	scp -o LogLevel=ERROR -q $(BUILD)/test_rate_limit $(QNX_HOST_SSH):$(QNX_DIR)/
	ssh -o LogLevel=ERROR $(QNX_HOST_SSH) "$(QNX_DIR)/test_rate_limit"

tarea4: check-env | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -I$(TAREA4) $(TAREA4)/plant_auth.c $(TAREA4)/test_plant_auth.c -o $(BUILD)/test_plant_auth
	ssh -o LogLevel=ERROR $(QNX_HOST_SSH) "mkdir -p $(QNX_DIR)"
	scp -o LogLevel=ERROR -q $(BUILD)/test_plant_auth $(QNX_HOST_SSH):$(QNX_DIR)/
	ssh -o LogLevel=ERROR $(QNX_HOST_SSH) "$(QNX_DIR)/test_plant_auth"

# Tarea 5: recorrer el árbol del copiloto. En la VM (make tarea5) o en Ubuntu (make tarea5-host).
TAREA5_SRC := $(TAREA5)/tree_predict.c $(TAREA5)/test_tree_predict.c
tarea5: check-env | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -I$(TAREA5) $(TAREA5_SRC) -lm -o $(BUILD)/test_tree_predict
	ssh -o LogLevel=ERROR $(QNX_HOST_SSH) "mkdir -p $(QNX_DIR)"
	scp -o LogLevel=ERROR -q $(BUILD)/test_tree_predict $(QNX_HOST_SSH):$(QNX_DIR)/
	ssh -o LogLevel=ERROR $(QNX_HOST_SSH) "$(QNX_DIR)/test_tree_predict"

tarea5-host: | $(BUILD)
	gcc -std=c11 -O2 -Wall -Wextra -Isrc -I$(TAREA5) $(TAREA5_SRC) -lm -o $(BUILD)/test_tree_predict_host
	$(BUILD)/test_tree_predict_host

all: check-env $(BINS) $(M3_BINS) $(M4_BINS) $(M5_BINS) $(M6_BINS) $(DESIGN_BINS) $(SEC_BINS) $(VENTD_BINS)

m3: check-env $(M3_BINS)

# pneumatic_sim enlaza la autorización por pid (tarea 4) del usuario.
$(BUILD)/pneumatic_sim: src/pneumatic_sim.c src/plant.c src/plant.h $(TAREA4)/plant_auth.c $(TAREA4)/plant_auth.h $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -I$(TAREA4) src/pneumatic_sim.c src/plant.c $(TAREA4)/plant_auth.c -lm -o $@

$(BUILD)/breath_ctrl: src/breath_ctrl.c src/vent.c src/vent.h src/plant.c src/settings.c $(TAREA3)/rate_limit.c $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc src/breath_ctrl.c src/vent.c src/plant.c src/settings.c $(TAREA3)/rate_limit.c -lm -o $@

m4: check-env $(M3_BINS) $(M4_BINS)

m5: check-env $(M3_BINS) $(M4_BINS) $(M5_BINS)

m6: check-env $(M3_BINS) $(M4_BINS) $(M5_BINS) $(M6_BINS)

# En QNX los sockets están en libsocket.
$(BUILD)/hmi: src/hmi.c src/settings.c $(TAREA3)/rate_limit.c src/bg_power.h $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc src/hmi.c src/settings.c $(TAREA3)/rate_limit.c -lsocket -lm -o $@

# Version monolitica (control + pantalla en un proceso) para el contraste de la demo.
$(BUILD)/mono_vent: src/mono_vent.c src/vent.c src/vent.h src/plant.c $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc src/mono_vent.c src/vent.c src/plant.c -lm -lsocket -o $@

$(BUILD)/power_mgr: src/power_mgr.c src/battery.c src/battery.h src/bg_power.h $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc src/power_mgr.c src/battery.c -lm -o $@

$(BUILD)/m5_offline: tools/m5_offline.c src/battery.c src/plant.c src/vent.c | $(BUILD)
	gcc -O2 -Wall -Wextra -Isrc $^ -lm -o $@

# safety_mon enlaza el watchdog (tarea 1) y el medidor de Vt (tarea 2) del usuario.
SAFETY_SRC := src/safety_mon.c src/alarms.c src/lung_mon.c src/vent.c src/plant.c src/settings.c $(TAREA3)/rate_limit.c $(TAREA1)/watchdog.c $(TAREA2)/vt_meter.c
$(BUILD)/safety_mon: $(SAFETY_SRC) src/bg_power.h src/alarms.h src/lung_mon.h src/vent.h $(TAREA1)/watchdog.h $(TAREA2)/vt_meter.h $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -I$(TAREA1) -I$(TAREA2) $(SAFETY_SRC) -lm -lsecpol -o $@

# /dev/ventilator: resource manager (punto A del plan QNX).
$(BUILD)/ventd: src/ventd.c src/settings.c src/alarms.c $(TAREA3)/rate_limit.c src/alarms.h src/bg_power.h $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc src/ventd.c src/settings.c src/alarms.c $(TAREA3)/rate_limit.c -lm -o $@

# HMI comprometida para la prueba de la tarea 4.
$(BUILD)/evil_hmi: src/evil_hmi.c $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc src/evil_hmi.c -o $@

$(BUILD)/bg_fault: src/bg_fault.c src/bg_power.h $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc src/bg_fault.c -o $@

# patient: Pulse para QNX, compilado aparte con port/build_pulse_qnx.sh (en ~/pulse/qnx).
PULSE_QNX := $(HOME)/pulse/qnx
PULSE_LIBS := $(PULSE_QNX)/qnx-pulse/src/cpp/libPulse.a $(PULSE_QNX)/staging/lib/libprotobuf.a

$(BUILD)/patient: src/patient.cpp $(COMMON_H) | $(BUILD)
	q++ -Vgcc_ntox86_64_cxx -std=c++20 -O2 -Wall -Wextra -Wno-deprecated-copy -D_QNX_SOURCE -Isrc \
	    -I$(HOME)/pulse/engine/src/cpp -I$(PULSE_QNX)/qnx-pulse/src/cpp -I$(PULSE_QNX)/staging/include \
	    src/patient.cpp $(PULSE_LIBS) -o $@
	ntox86_64-strip $@

# Versión de Ubuntu (sin QNX) de la planta + PCV, para ajustar y como referencia.
$(BUILD)/m3_offline: tools/m3_offline.c src/plant.c src/vent.c | $(BUILD)
	gcc -O2 -Wall -Wextra -Isrc $^ -lm -o $@

$(BUILD)/m4_offline: tools/m4_offline.c src/plant.c src/vent.c src/alarms.c | $(BUILD)
	gcc -O2 -Wall -Wextra -Isrc $^ -lm -o $@

# design/: escalones en lazo abierto sobre pneumatic_sim (QNX) y la referencia ideal en el host.
$(BUILD)/step_test: src/step_test.c src/plant.c $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc src/step_test.c src/plant.c -lm -o $@

$(BUILD)/design_ref: tools/design_ref.c src/plant.c src/vent.c | $(BUILD)
	gcc -O2 -Wall -Wextra -Isrc $^ -lm -o $@

# Prueba en el host de la regla de reemplazo de ajustes (settings_schedule).
$(BUILD)/settings_schedule_test: tools/settings_schedule_test.c src/settings.c src/settings.h $(TAREA3)/rate_limit.c | $(BUILD)
	gcc -std=c11 -O2 -Wall -Wextra -Isrc $(filter %.c,$^) -lm -o $@

# Prueba en el host del estimador de auto-PEEP contra la presión alveolar verdadera de la planta.
$(BUILD)/lung_mon_test: tools/lung_mon_test.c src/lung_mon.c src/plant.c src/vent.c src/settings.c $(TAREA3)/rate_limit.c src/lung_mon.h | $(BUILD)
	gcc -O2 -Wall -Wextra -Isrc $(filter %.c,$^) -lm -o $@

# Copiloto (punto 5): datos de entrenamiento del banco offline (misma planta, ventilador y features que en QNX).
$(BUILD)/copilot_gen: tools/copilot_gen.c src/breath_feat.c src/plant.c src/vent.c src/settings.c $(TAREA3)/rate_limit.c src/breath_feat.h | $(BUILD)
	gcc -O2 -Wall -Wextra -Isrc $(filter %.c,$^) -lm -o $@

# Prueba en el host de las reglas fijas del copiloto y de la validación de safety_mon.
$(BUILD)/copilot_logic_test: tools/copilot_logic_test.c src/copilot_logic.c src/settings.c $(TAREA3)/rate_limit.c src/copilot_logic.h src/breath_feat.h | $(BUILD)
	gcc -std=c11 -O2 -Wall -Wextra -Isrc $(filter %.c,$^) -lm -o $@

copilot-logic-test: $(BUILD)/copilot_logic_test
	$(BUILD)/copilot_logic_test

check-env:
	@command -v $(CC) >/dev/null || { echo "Falta qcc: source ~/qnx800/qnxsdp-env.sh"; exit 1; }

$(BUILD)/%: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD):
	mkdir -p $@

deploy: all
	ssh $(QNX_HOST_SSH) "mkdir -p $(QNX_DIR)"
	scp -q $(BINS) $(M3_BINS) $(M4_BINS) $(M5_BINS) $(M6_BINS) $(SEC_BINS) $(VENTD_BINS) $(QNX_HOST_SSH):$(QNX_DIR)/

clean:
	rm -rf $(BUILD)
