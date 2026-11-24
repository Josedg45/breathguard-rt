#!/bin/sh
# bench_target.sh - runs on the QNX VM: the interactive test bench (full ventilator + Pulse patient, no time limit).
# The host drives it over SSH (scripts/bench.py, the dashboard's bench controls). Bench actions are the physical
# world and the test operator: they do NOT go through the HMI, which is the ventilator's operator interface.
# use: bench_target.sh start <host_ip> [patient_id]   stop whatever runs, start everything, return at once
#      bench_target.sh patient <patient_id>           swap the virtual patient (Pulse) while ventilating
#      bench_target.sh fault [bg_fault options]        inject faults (no options: clear them all)
#      bench_target.sh mains 0|1                       mains power off / restored
#      bench_target.sh kill hmi|ctrl|copilot           kill one process (SIGKILL), as in the demo scenes
#      bench_target.sh status                          which processes run and which patient is connected
#      bench_target.sh stop                            stop the bench
# Patients: /data/pulse/states/BG_<id>.json (tools/pulse_states, copied by scripts/bench.py deploy).
cd "$(dirname "$0")" || exit 1
D=$(pwd)
PULSE=/data/pulse
LOG=$D/bench_logs
mkdir -p "$LOG"

# Started from a short SSH session: ignore SIGHUP (inherited across exec) and detach stdio, or the processes would
# die, or keep the session open, when it ends.
detach() {
    ( trap '' HUP; exec "$@" < /dev/null ) &
}

start_patient() {
    id=$1
    f=$PULSE/states/BG_$id.json
    [ -f "$f" ] || { echo "bench: no patient '$id' ($f missing)"; return 1; }
    slay -s TERM patient > /dev/null 2>&1 || true
    sleep 1
    slay -s KILL patient > /dev/null 2>&1 || true
    cd $PULSE || return 1
    detach "$D/patient" -s 0 -S "states/BG_$id.json" > "$LOG/patient.txt" 2>&1
    cd "$D" || return 1
    echo "bench: patient $id"
}

case "$1" in
start)
    HOST=${2:?host ip}
    ID=${3:-healthy_male}
    [ -x /system/bg/bg_stop.sh ] && /system/bg/bg_stop.sh
    slay -s TERM safety_mon pneumatic_sim power_mgr patient ventd > /dev/null 2>&1 || true
    sleep 1
    rm -f "$LOG"/*.txt
    detach ./pneumatic_sim -s 0 -e 5 > "$LOG/sim.txt" 2>&1
    sleep 1
    detach ./safety_mon -s 0 -H "$HOST" -K -o "$LOG/events.csv" > "$LOG/safety.txt" 2>&1
    detach ./power_mgr -s 0 -x 60 > "$LOG/power.txt" 2>&1
    detach ./ventd > "$LOG/ventd.txt" 2>&1
    start_patient "$ID"
    ;;
patient)
    start_patient "${2:?patient id}"
    ;;
fault)
    shift
    ./bg_fault "$@"
    ;;
mains)
    ./bg_fault -m "${2:?0|1}"
    ;;
kill)
    case "$2" in
        hmi) slay -s KILL hmi ;;
        ctrl) ./bg_fault -n; slay -s KILL breath_ctrl ;;
        copilot) slay -s KILL copilot ;;
        *) echo "bench: kill hmi|ctrl|copilot"; exit 1 ;;
    esac > /dev/null 2>&1
    echo "bench: killed $2"
    ;;
status)
    pidin -f an | grep -E "pneumatic_sim|breath_ctrl|safety_mon|power_mgr|ventd|hmi|copilot|patient" | sort -u
    tail -n 2 "$LOG/patient.txt" 2>/dev/null
    ;;
stop)
    slay -s TERM safety_mon power_mgr ventd patient > /dev/null 2>&1 || true
    sleep 1
    slay -s TERM pneumatic_sim > /dev/null 2>&1 || true
    echo "bench: stopped (reboot the VM to get the boot ventilator back)"
    ;;
*)
    echo "use: bench_target.sh start|patient|fault|mains|kill|status|stop ..."
    exit 1
    ;;
esac
exit 0
