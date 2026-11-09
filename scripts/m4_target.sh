#!/bin/sh
# m4_target.sh - corre en la VM QNX una escena de seguridad con los 4 procesos:
#   patient (Pulse, prio 20), pneumatic_sim (60), safety_mon (55), que lanza breath_ctrl (50).
# uso: m4_target.sh etiqueta segundos escena [opciones de safety_mon...]
# Escenas (a los T_FALLA s del arranque de safety_mon):
#   normal      sin fallas
#   matar       slay -s KILL breath_ctrl (el proceso muere)
#   congelar    slay -s STOP breath_ctrl (sigue vivo pero no corre: cuelgue)
#   desconectar bg_fault -d 1, y se reconecta 15 s después
#   obstruir    bg_fault -o 8 (resistencia x8), y se quita 15 s después
#   carga_matar como matar, con la CPU saturada (2 hilos a prio 9)
cd "$(dirname "$0")" || exit 1
# El ventilador que arranca con la imagen (punto C) ocupa los nombres: se detiene antes de la prueba.
[ -x /system/bg/bg_stop.sh ] && /system/bg/bg_stop.sh
D=$(pwd)
L=$1; S=$2; ESC=$3; shift 3
T_FALLA=40
rm -f log_*_$L.txt
(cd /data/pulse && "$D/patient" -s $((S + 2)) -o "$D/pat_$L.csv" > "$D/log_pat_$L.txt" 2>&1) &
sleep 2
./pneumatic_sim -s $((S + 4)) -e 5 -o "sim_$L.csv" > "log_sim_$L.txt" &
sleep 1
./safety_mon -s "$S" -o "ev_$L.csv" "$@" > "log_safety_$L.txt" 2>&1 &
if [ "$ESC" = "carga_matar" ]; then
    sleep 2
    ./cpu_burn -s $((S - 3)) -p 9 > "log_burn_$L.txt" &
    sleep $((T_FALLA - 2))
else
    sleep $T_FALLA
fi
case "$ESC" in
    matar|carga_matar) ./bg_fault -n; slay -s KILL breath_ctrl ;;
    congelar) ./bg_fault -n; slay -s STOP breath_ctrl ;;
    desconectar) ./bg_fault -d 1; sleep 15; ./bg_fault ;;
    obstruir) ./bg_fault -o 8; sleep 15; ./bg_fault ;;
esac > "log_escena_$L.txt" 2>&1
wait
cat log_*_$L.txt
true
