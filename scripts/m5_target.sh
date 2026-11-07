#!/bin/sh
# m5_target.sh - corre en la VM QNX la escena de energía: el sistema completo + power_mgr.
# uso: m5_target.sh etiqueta segundos escala soc_inicial
#   Corte de red a los 30 s y vuelta a los 75 s (si la corrida dura lo suficiente).
#   escala: la batería corre 'escala' veces más rápido (60 = 1 s real es 1 min de batería).
cd "$(dirname "$0")" || exit 1
D=$(pwd)
L=$1; S=$2; X=$3; B=$4
rm -f log_*_$L.txt
(cd /data/pulse && "$D/patient" -s $((S + 2)) -o "$D/pat_$L.csv" > "$D/log_pat_$L.txt" 2>&1) &
sleep 2
./pneumatic_sim -s $((S + 4)) -e 5 -o "sim_$L.csv" > "log_sim_$L.txt" &
sleep 1
./safety_mon -s "$S" -o "ev_$L.csv" > "log_safety_$L.txt" 2>&1 &
./power_mgr -s "$S" -x "$X" -b "$B" -o "pwr_$L.csv" > "log_pwr_$L.txt" 2>&1 &
sleep 30
./bg_fault -m 0 > "log_escena_$L.txt" 2>&1
if [ "$S" -gt 80 ]; then
    sleep 45
    ./bg_fault -m 1 >> "log_escena_$L.txt" 2>&1
fi
wait
cat log_*_$L.txt
true
