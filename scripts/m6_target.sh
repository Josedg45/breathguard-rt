#!/bin/sh
# m6_target.sh - corre en la VM QNX el sistema completo con HMI y una escena sobre la HMI.
# uso: m6_target.sh etiqueta segundos escena ip_host
# Escenas (a los 30 s): normal | hmi_matar (KILL) | hmi_bug (SIGUSR1: puntero nulo) | hmi_colgar (SIGUSR2: bucle)
# ajustes: el host cambia los ajustes por UDP (m6_run.sh) y a los 55 s se mata breath_ctrl
# autopeep: el host pone 30 rpm con I:E 1:1 (espiracion de 1 s) y luego vuelve a 12 rpm 1:2
# Contraste con la version monolitica (mono_vent, sin safety_mon): mono_normal | mono_bug | mono_colgar
cd "$(dirname "$0")" || exit 1
D=$(pwd)
L=$1; S=$2; ESC=$3; HOST=$4
rm -f log_*_$L.txt
(cd /data/pulse && "$D/patient" -s $((S + 2)) -o "$D/pat_$L.csv" > "$D/log_pat_$L.txt" 2>&1) &
sleep 2
./pneumatic_sim -s $((S + 4)) -e 5 -o "sim_$L.csv" > "log_sim_$L.txt" &
sleep 1
case "$ESC" in
    mono_*)
        ./mono_vent -s "$S" -h "$HOST" > "log_mono_$L.txt" 2>&1 &
        echo "t_s,code,info" > "ev_$L.csv" ;;  # el monolito no registra eventos
    *)
        ./safety_mon -s "$S" -H "$HOST" -o "ev_$L.csv" > "log_safety_$L.txt" 2>&1 &
        ./power_mgr -s "$S" -x 60 > "log_pwr_$L.txt" 2>&1 & ;;
esac
sleep 30
case "$ESC" in
    mono_bug) ./bg_fault -n; slay -s USR1 mono_vent ;;
    mono_colgar) ./bg_fault -n; slay -s USR2 mono_vent
        (sleep $((S - 28)); slay -s KILL mono_vent) & ;;  # colgado no termina solo
    hmi_matar) ./bg_fault -n; slay -s KILL hmi ;;
    ajustes) sleep 25; ./bg_fault -n; slay -s KILL breath_ctrl ;;  # los ajustes los manda el host
    hmi_bug) ./bg_fault -n; slay -s USR1 hmi ;;
    hmi_colgar) ./bg_fault -n; slay -s USR2 hmi ;;
esac > "log_escena_$L.txt" 2>&1
wait
cat log_*_$L.txt
true
