#!/bin/sh
# b2_target.sh - runs on the QNX VM (image built by scripts/image_build.sh, with the security policy).
# Starts the ventilator exactly as the boot does (same secpol types, binaries from /system/bg) and checks layer 2:
#   1. every process runs with its type (pidin -f an_)
#   2. a compromised HMI (evil_hmi with the HMI type) cannot even connect to the plant: the kernel refuses
#   3. the same with the ventd type (ventd has no reason to talk to the plant either)
#   4. the HMI type cannot write /dev/ventilator/settings (not in its channel_connect list)
#   5. the legitimate stack keeps working: no denials of our own types, settings write from root OK, Paw unchanged
# Leaves the ventilator running, as after boot. Usage: b2_target.sh
[ -x /system/bg/bg_stop.sh ] && /system/bg/bg_stop.sh
cd /system/bg || exit 1
LOG=/data/var/log/bg; mkdir -p $LOG
secpolgenerate -C clear errors > /dev/null 2>&1
on -T bg_plant_t ./pneumatic_sim -s 0 > $LOG/plant.txt 2>&1 &
waitfor /dev/name/local/breathguard_pneumatic 5
on -T bg_safety_t ./safety_mon -s 0 -H 172.31.1.1 -o $LOG/events.csv > $LOG/safety.txt 2>&1 &
on -T bg_power_t ./power_mgr -s 0 > $LOG/power.txt 2>&1 &
on -T bg_ventd_t ./ventd > $LOG/ventd.txt 2>&1 &
cd /
sleep 15

echo "== 1. types"
pidin -f anN_ | grep bg_
echo "== status before"
cat /dev/ventilator/status
echo "MARK attack $(date +%s)"
echo "== 2. evil_hmi as bg_safety_t__hmi"
on -T bg_safety_t__hmi /system/bg/evil_hmi -d 2
echo "RC_HMI $?"
echo "== 3. evil_hmi as bg_ventd_t"
on -T bg_ventd_t /system/bg/evil_hmi -d 2
echo "RC_VENTD $?"
echo "== 4. settings write as bg_safety_t__hmi"
on -T bg_safety_t__hmi /proc/boot/ksh -c 'echo "p_insp=21" > /dev/ventilator/settings' 2>&1
echo "RC_HMI_WRITE $?"
echo "== 5. settings write as root (unrestricted, the operator's console)"
echo "p_insp=21" > /dev/ventilator/settings
echo "RC_ROOT_WRITE $?"
sleep 12
echo "== status after"
cat /dev/ventilator/status
echo "== settings"
cat /dev/ventilator/settings
echo "== secpolgenerate errors (kernel denials)"
cat /dev/secpolgenerate/errors
echo "== end"
