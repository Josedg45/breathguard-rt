#!/bin/sh
# demo_target.sh - corre en la VM QNX el sistema completo para la demo. Las escenas las dispara el host
# (scripts/demo_run.sh) por SSH, para que el video y los subtitulos vayan sincronizados.
# uso: demo_target.sh etiqueta segundos ip_host
cd "$(dirname "$0")" || exit 1
D=$(pwd)
L=$1; S=$2; HOST=$3
rm -f log_*_$L.txt
(cd /data/pulse && "$D/patient" -s $((S + 2)) -o "$D/pat_$L.csv" > "$D/log_pat_$L.txt" 2>&1) &
sleep 2
./pneumatic_sim -s $((S + 4)) -e 5 -o "sim_$L.csv" > "log_sim_$L.txt" &
sleep 1
./safety_mon -s "$S" -H "$HOST" -o "ev_$L.csv" > "log_safety_$L.txt" 2>&1 &
./power_mgr -s "$S" -x 60 -o "pwr_$L.csv" > "log_pwr_$L.txt" 2>&1 &
wait
cat log_*_$L.txt
true
