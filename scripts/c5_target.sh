#!/bin/sh
# c5_target.sh - runs in the QNX VM: the copilot (point 5) in closed loop, with faults injected at known times.
#   rc     RC plant (no Pulse), PCV 16/5: normal 40 s -> leak -> obstruction (the copilot is killed in the middle)
#          -> stiff lung; the operator accepts the proposal through /dev/ventilator/copilot.
#   pulse  Pulse as the patient, PCV 20/5 (the demo settings): normal 60 s -> leak 60 s -> obstruction 60 s.
#   liar   RC plant with the test-only liar copilot: everything it proposes must be blocked.
# use: c5_target.sh label scene
cd "$(dirname "$0")" || exit 1
[ -x /system/bg/bg_stop.sh ] && /system/bg/bg_stop.sh
D=$(pwd)
L=$1; ESC=$2
rm -f log_*_$L.txt cop_$L.csv
mark() { echo "MARK $1 $(./bg_fault -n | sed -n 's/.*t_abs=\([0-9.]*\).*/\1/p')"; }
show() { echo "== copilot ($1)"; cat /dev/ventilator/copilot; }
case "$ESC" in
    pulse) S=220; P=20; COP=./copilot ;;
    liar) S=60; P=16; COP=./liar_copilot ;;
    *) S=190; P=16; COP=./copilot ;;
esac
if [ "$ESC" = pulse ]; then
    (cd /data/pulse && "$D/patient" -s $((S + 2)) -o "$D/pat_$L.csv" > "$D/log_pat_$L.txt" 2>&1) &
    sleep 2
fi
./pneumatic_sim -s $((S + 4)) -e 5 -o "sim_$L.csv" > "log_sim_$L.txt" &
sleep 1
./safety_mon -s "$S" -m pcv -P $P -e 5 -K -k $COP -Q "cop_$L.csv" -o "ev_$L.csv" > "log_safety_$L.txt" 2>&1 &
./ventd -s $((S - 1)) > "log_ventd_$L.txt" 2>&1 &
{
mark start
case "$ESC" in
    rc)
        sleep 40; mark leak; ./bg_fault -l 0.004
        sleep 30; show leak
        sleep 10; mark clear; ./bg_fault
        sleep 5; mark obstruction; ./bg_fault -o 3
        sleep 20; mark kill_copilot; slay -s KILL copilot > /dev/null || true
        sleep 15; show obstruction
        sleep 5; mark clear; ./bg_fault
        sleep 5; mark stiff; ./bg_fault -c 2.5
        sleep 25; show stiff
        id=$(sed -n 's/^proposal: id=\([0-9]*\) state=pending.*/\1/p' /dev/ventilator/copilot)
        echo "== accept $id"; echo "accept $id" > /dev/ventilator/copilot; echo "RC_ACCEPT $?"
        echo "== accept $id again"; echo "accept $id" > /dev/ventilator/copilot; echo "RC_ACCEPT_AGAIN $?"
        echo "== bad command"; echo "approve all" > /dev/ventilator/copilot; echo "RC_BAD $?"
        sleep 3; echo "== settings"; cat /dev/ventilator/settings
        sleep 25; show end
        ;;
    pulse)
        sleep 60; mark leak; ./bg_fault -l 0.004
        sleep 55; show leak
        sleep 5; mark clear; ./bg_fault
        sleep 10; mark obstruction; ./bg_fault -o 3
        sleep 55; show obstruction
        sleep 5; mark clear; ./bg_fault
        sleep 25; show end
        ;;
    liar)
        sleep 30; show liar
        sleep 25; show end
        ;;
esac
} > "log_escena_$L.txt" 2>&1
wait
cat log_escena_$L.txt log_safety_$L.txt log_ventd_$L.txt
true
