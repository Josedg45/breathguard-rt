# BreathGuard RT - build for QNX SDP 8.0, target QEMU x86_64 (toolchain qcc/q++ gcc_ntox86_64).
# First: source ~/qnx800/qnxsdp-env.sh

CC      := qcc
TARGET  := gcc_ntox86_64
CFLAGS  := -V$(TARGET) -Wall -Wextra -O2 -g

QNX_HOST_SSH := root@172.31.1.2
QNX_DIR      := /tmp/medpower
SSH := ssh -o LogLevel=ERROR
SCP := scp -o LogLevel=ERROR -q

BUILD := build
BINS  := $(BUILD)/latency_test $(BUILD)/cpu_burn $(BUILD)/vm_noise

# Ventilator processes (several sources each)
M3_BINS := $(BUILD)/pneumatic_sim $(BUILD)/breath_ctrl
M4_BINS := $(BUILD)/safety_mon $(BUILD)/bg_fault
M5_BINS := $(BUILD)/power_mgr
M6_BINS := $(BUILD)/hmi $(BUILD)/mono_vent
DESIGN_BINS := $(BUILD)/step_test
SEC_BINS := $(BUILD)/evil_hmi
VENTD_BINS := $(BUILD)/ventd
COPILOT_BINS := $(BUILD)/copilot $(BUILD)/liar_copilot
COMMON_H := src/bg_types.h src/bg_proto.h src/rt_util.h src/settings.h src/copilot_logic.h src/breath_feat.h
SETTINGS_SRC := src/settings.c src/rate_limit.c

# Unit tests of the safety-critical modules. `make test` runs them on the QNX target, `make test-host` on Ubuntu.
UNIT_TESTS := test_watchdog test_vt_meter test_rate_limit test_plant_auth test_tree_predict
TEST_watchdog_SRC := src/watchdog.c tests/test_watchdog.c
TEST_vt_meter_SRC := src/vt_meter.c tests/test_vt_meter.c
TEST_rate_limit_SRC := src/rate_limit.c tests/test_rate_limit.c
TEST_plant_auth_SRC := src/plant_auth.c tests/test_plant_auth.c
TEST_tree_predict_SRC := src/tree_predict.c tests/test_tree_predict.c

.PHONY: all clean deploy check-env test test-host copilot-logic-test m3 m4 m5 m6 patient

all: check-env $(BINS) $(M3_BINS) $(M4_BINS) $(M5_BINS) $(M6_BINS) $(DESIGN_BINS) $(SEC_BINS) $(VENTD_BINS) $(COPILOT_BINS)

patient: check-env $(BUILD)/patient

$(BUILD)/test_%: tests/test_%.c src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itests $(TEST_$*_SRC) -lm -o $@

$(BUILD)/host_test_%: tests/test_%.c src/%.c | $(BUILD)
	gcc -std=c11 -O2 -Wall -Wextra -Isrc -Itests $(TEST_$*_SRC) -lm -o $@

test: check-env $(addprefix $(BUILD)/,$(UNIT_TESTS))
	$(SSH) $(QNX_HOST_SSH) "mkdir -p $(QNX_DIR)"
	$(SCP) $(addprefix $(BUILD)/,$(UNIT_TESTS)) $(QNX_HOST_SSH):$(QNX_DIR)/
	$(SSH) $(QNX_HOST_SSH) 'set -e; for t in $(UNIT_TESTS); do echo "== $$t"; $(QNX_DIR)/$$t; done; echo "ALL UNIT TESTS PASSED"'

# test_plant_auth uses QNX's struct _msg_info, so it only builds for the target.
test-host: $(addprefix $(BUILD)/host_,$(filter-out test_plant_auth,$(UNIT_TESTS)))
	set -e; for t in $^; do echo "== $$t"; $$t; done; echo "ALL HOST UNIT TESTS PASSED"

m3: check-env $(M3_BINS)

m4: check-env $(M3_BINS) $(M4_BINS)

m5: check-env $(M3_BINS) $(M4_BINS) $(M5_BINS)

m6: check-env $(M3_BINS) $(M4_BINS) $(M5_BINS) $(M6_BINS)

# The plant accepts actuator commands only from the pid safety_mon authorizes (src/plant_auth.c).
$(BUILD)/pneumatic_sim: src/pneumatic_sim.c src/plant.c src/plant.h src/plant_auth.c src/plant_auth.h $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc src/pneumatic_sim.c src/plant.c src/plant_auth.c -lm -o $@

$(BUILD)/breath_ctrl: src/breath_ctrl.c src/vent.c src/vent.h src/plant.c $(SETTINGS_SRC) $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc src/breath_ctrl.c src/vent.c src/plant.c $(SETTINGS_SRC) -lm -o $@

# On QNX the sockets live in libsocket.
$(BUILD)/hmi: src/hmi.c src/copilot_logic.c $(SETTINGS_SRC) src/bg_power.h $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc src/hmi.c src/copilot_logic.c $(SETTINGS_SRC) -lsocket -lm -o $@

# Monolithic build (control + display in one process) for the demo contrast.
$(BUILD)/mono_vent: src/mono_vent.c src/vent.c src/vent.h src/plant.c $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc src/mono_vent.c src/vent.c src/plant.c -lm -lsocket -o $@

$(BUILD)/power_mgr: src/power_mgr.c src/battery.c src/battery.h src/bg_power.h $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc src/power_mgr.c src/battery.c -lm -o $@

$(BUILD)/m5_offline: tools/m5_offline.c src/battery.c src/plant.c src/vent.c | $(BUILD)
	gcc -O2 -Wall -Wextra -Isrc $^ -lm -o $@

# safety_mon: heartbeat watchdog (src/watchdog.c) and tidal volume meter (src/vt_meter.c).
SAFETY_SRC := src/safety_mon.c src/alarms.c src/lung_mon.c src/breath_feat.c src/copilot_logic.c src/vent.c src/plant.c $(SETTINGS_SRC) src/watchdog.c src/vt_meter.c
$(BUILD)/safety_mon: $(SAFETY_SRC) src/bg_power.h src/alarms.h src/lung_mon.h src/vent.h src/watchdog.h src/vt_meter.h $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $(SAFETY_SRC) -lm -lsecpol -lslog2 -o $@

# /dev/ventilator: resource manager.
$(BUILD)/ventd: src/ventd.c src/alarms.c src/copilot_logic.c $(SETTINGS_SRC) src/alarms.h src/bg_power.h $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc src/ventd.c src/alarms.c src/copilot_logic.c $(SETTINGS_SRC) -lm -o $@

# Copilot: learned decision tree (src/copilot_tree.h, walked by src/tree_predict.c) + fixed rules.
COPILOT_SRC := src/copilot.c src/copilot_logic.c src/breath_feat.c $(SETTINGS_SRC) src/tree_predict.c
$(BUILD)/copilot: $(COPILOT_SRC) src/copilot_tree.h src/tree_predict.h $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $(COPILOT_SRC) -lm -o $@

# Tests only (never in the image): a copilot that lies and tries to confirm itself and open the plant.
$(BUILD)/liar_copilot: $(COPILOT_SRC) src/copilot_tree.h src/tree_predict.h $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -DCOPILOT_LIAR -Isrc $(COPILOT_SRC) -lm -o $@

# Compromised HMI for the plant authorization test.
$(BUILD)/evil_hmi: src/evil_hmi.c $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc src/evil_hmi.c -o $@

$(BUILD)/bg_fault: src/bg_fault.c src/bg_power.h $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc src/bg_fault.c -o $@

# patient: Pulse for QNX, built separately with port/build_pulse_qnx.sh (in ~/pulse/qnx).
PULSE_QNX := $(HOME)/pulse/qnx
PULSE_LIBS := $(PULSE_QNX)/qnx-pulse/src/cpp/libPulse.a $(PULSE_QNX)/staging/lib/libprotobuf.a

$(BUILD)/patient: src/patient.cpp $(COMMON_H) | $(BUILD)
	q++ -Vgcc_ntox86_64_cxx -std=c++20 -O2 -Wall -Wextra -Wno-deprecated-copy -D_QNX_SOURCE -Isrc \
	    -I$(HOME)/pulse/engine/src/cpp -I$(PULSE_QNX)/qnx-pulse/src/cpp -I$(PULSE_QNX)/staging/include \
	    src/patient.cpp $(PULSE_LIBS) -o $@
	ntox86_64-strip $@

# Ubuntu (non-QNX) build of the plant + PCV, for tuning and as a reference.
$(BUILD)/m3_offline: tools/m3_offline.c src/plant.c src/vent.c | $(BUILD)
	gcc -O2 -Wall -Wextra -Isrc $^ -lm -o $@

$(BUILD)/m4_offline: tools/m4_offline.c src/plant.c src/vent.c src/alarms.c | $(BUILD)
	gcc -O2 -Wall -Wextra -Isrc $^ -lm -o $@

# design/: open-loop steps on pneumatic_sim (QNX) and the ideal reference on the host.
$(BUILD)/step_test: src/step_test.c src/plant.c $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc src/step_test.c src/plant.c -lm -o $@

$(BUILD)/design_ref: tools/design_ref.c src/plant.c src/vent.c | $(BUILD)
	gcc -O2 -Wall -Wextra -Isrc $^ -lm -o $@

# Host test of the settings replacement rule (settings_schedule).
$(BUILD)/settings_schedule_test: tools/settings_schedule_test.c $(SETTINGS_SRC) src/settings.h | $(BUILD)
	gcc -std=c11 -O2 -Wall -Wextra -Isrc $(filter %.c,$^) -lm -o $@

# Host test of the auto-PEEP estimator against the plant's true alveolar pressure.
$(BUILD)/lung_mon_test: tools/lung_mon_test.c src/lung_mon.c src/plant.c src/vent.c $(SETTINGS_SRC) src/lung_mon.h | $(BUILD)
	gcc -O2 -Wall -Wextra -Isrc $(filter %.c,$^) -lm -o $@

# Copilot training data from the offline bench (same plant, ventilator and features as on QNX).
$(BUILD)/copilot_gen: tools/copilot_gen.c src/breath_feat.c src/plant.c src/vent.c $(SETTINGS_SRC) src/breath_feat.h | $(BUILD)
	gcc -O2 -Wall -Wextra -Isrc $(filter %.c,$^) -lm -o $@

# Host test of the copilot's fixed rules and of safety_mon's validation.
$(BUILD)/copilot_logic_test: tools/copilot_logic_test.c src/copilot_logic.c $(SETTINGS_SRC) src/copilot_logic.h src/breath_feat.h | $(BUILD)
	gcc -std=c11 -O2 -Wall -Wextra -Isrc $(filter %.c,$^) -lm -o $@

copilot-logic-test: $(BUILD)/copilot_logic_test
	$(BUILD)/copilot_logic_test

check-env:
	@command -v $(CC) >/dev/null || { echo "qcc not found: source ~/qnx800/qnxsdp-env.sh"; exit 1; }

$(BUILD)/%: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD):
	mkdir -p $@

deploy: all
	$(SSH) $(QNX_HOST_SSH) "mkdir -p $(QNX_DIR)"
	$(SCP) $(BINS) $(M3_BINS) $(M4_BINS) $(M5_BINS) $(M6_BINS) $(SEC_BINS) $(VENTD_BINS) $(COPILOT_BINS) $(QNX_HOST_SSH):$(QNX_DIR)/

clean:
	rm -rf $(BUILD)
