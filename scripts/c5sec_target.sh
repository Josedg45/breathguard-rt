#!/bin/sh
# c5sec_target.sh - runs on the QNX VM (image with the security policy). Layer 2 for the copilot (point 5):
# the stack starts exactly as at boot, but safety_mon launches the test-only liar as its copilot. With the type
# bg_safety_t__copilot the kernel must refuse it the plant and /dev/ventilator/copilot; safety_mon must refuse its
# self-accepts (layer 1, by pid) and block its proposals (fixed rules). Then the normal stack is started again.
[ -x /system/bg/bg_stop.sh ] && /system/bg/bg_stop.sh
cd /system/bg || exit 1
LOG=/data/var/log/bg; mkdir -p $LOG
secpolgenerate -C clear errors > /dev/null 2>&1
on -T bg_plant_t ./pneumatic_sim -s 0 > $LOG/plant.txt 2>&1 &
waitfor /dev/name/local/breathguard_pneumatic 5
on -T bg_safety_t ./safety_mon -s 0 -m pcv -P 16 -K -k ./liar_copilot -o $LOG/events_c5sec.csv > $LOG/safety_c5sec.txt 2>&1 &
on -T bg_ventd_t ./ventd > $LOG/ventd.txt 2>&1 &
cd /
sleep 25
echo "== types"
pidin -f anN_ | grep bg_
echo "== copilot"
cat /dev/ventilator/copilot
echo "== a shell with the copilot type writes accept"
on -T bg_safety_t__copilot /proc/boot/ksh -c 'echo "accept 1" > /dev/ventilator/copilot' 2>&1
echo "RC_COPILOT_TYPE_WRITE $?"
echo "== root (the operator's console) reads the copilot file"
cat /dev/ventilator/copilot > /dev/null
echo "RC_ROOT_READ $?"
echo "== secpolgenerate errors (kernel denials)"
cat /dev/secpolgenerate/errors
/system/bg/bg_stop.sh
sleep 1
echo "== safety_mon log"
cat $LOG/safety_c5sec.txt
echo "== events"
cat $LOG/events_c5sec.csv
# back to the boot stack (same lines as image/startup_postfs.custom)
cd /system/bg
on -T bg_plant_t ./pneumatic_sim -s 0 > $LOG/plant.txt 2>&1 &
waitfor /dev/name/local/breathguard_pneumatic 5
on -T bg_safety_t ./safety_mon -s 0 -H 172.31.1.1 -K -o $LOG/events.csv > $LOG/safety.txt 2>&1 &
on -T bg_power_t ./power_mgr -s 0 > $LOG/power.txt 2>&1 &
on -T bg_ventd_t ./ventd > $LOG/ventd.txt 2>&1 &
sleep 3
echo "== boot stack again"
pidin -f anN_ | grep bg_
echo "== end"
