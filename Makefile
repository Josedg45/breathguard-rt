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

# M3: procesos del ventilador (varios fuentes cada uno)
M3_BINS := $(BUILD)/pneumatic_sim $(BUILD)/breath_ctrl
M4_BINS := $(BUILD)/safety_mon $(BUILD)/bg_fault
M5_BINS := $(BUILD)/power_mgr
COMMON_H := src/bg_types.h src/bg_proto.h src/rt_util.h

.PHONY: all clean deploy check-env tarea1 tarea2 m3 m4 m5 patient

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

all: check-env $(BINS) $(M3_BINS) $(M4_BINS) $(M5_BINS)

m3: check-env $(M3_BINS)

$(BUILD)/pneumatic_sim: src/pneumatic_sim.c src/plant.c src/plant.h $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc src/pneumatic_sim.c src/plant.c -lm -o $@

$(BUILD)/breath_ctrl: src/breath_ctrl.c src/vent.c src/vent.h src/plant.c $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc src/breath_ctrl.c src/vent.c src/plant.c -lm -o $@

m4: check-env $(M3_BINS) $(M4_BINS)

m5: check-env $(M3_BINS) $(M4_BINS) $(M5_BINS)

$(BUILD)/power_mgr: src/power_mgr.c src/battery.c src/battery.h src/bg_power.h $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc src/power_mgr.c src/battery.c -lm -o $@

$(BUILD)/m5_offline: tools/m5_offline.c src/battery.c src/plant.c src/vent.c | $(BUILD)
	gcc -O2 -Wall -Wextra -Isrc $^ -lm -o $@

# safety_mon enlaza el watchdog (tarea 1) y el medidor de Vt (tarea 2) del usuario.
SAFETY_SRC := src/safety_mon.c src/alarms.c src/vent.c src/plant.c $(TAREA1)/watchdog.c $(TAREA2)/vt_meter.c
$(BUILD)/safety_mon: $(SAFETY_SRC) src/alarms.h src/vent.h $(TAREA1)/watchdog.h $(TAREA2)/vt_meter.h $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -I$(TAREA1) -I$(TAREA2) $(SAFETY_SRC) -lm -o $@

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

check-env:
	@command -v $(CC) >/dev/null || { echo "Falta qcc: source ~/qnx800/qnxsdp-env.sh"; exit 1; }

$(BUILD)/%: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD):
	mkdir -p $@

deploy: all
	ssh $(QNX_HOST_SSH) "mkdir -p $(QNX_DIR)"
	scp -q $(BINS) $(M3_BINS) $(M4_BINS) $(M5_BINS) $(QNX_HOST_SSH):$(QNX_DIR)/

clean:
	rm -rf $(BUILD)
