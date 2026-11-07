#!/bin/sh
# m3_target.sh - corre en la VM QNX: pneumatic_sim + breath_ctrl con unos ajustes de PCV.
# uso: m3_target.sh etiqueta p_insp peep rr segundos [carga]
# Con "carga": 2 hilos basura (uno por vCPU) a prio 9, lanzados DESPUÉS del lazo (lección de M1).
cd "$(dirname "$0")" || exit 1
L=$1; P=$2; E=$3; R=$4; S=$5; LOAD=$6
./pneumatic_sim -s $((S + 3)) -e "$E" -o "sim_$L.csv" > "log_sim_$L.txt" &
sleep 1   # la planta arranca primero y publica su nombre
./breath_ctrl -s "$S" -P "$P" -e "$E" -r "$R" -o "ctrl_$L.csv" > "log_ctrl_$L.txt" &
if [ "$LOAD" = "carga" ]; then
    sleep 1
    ./cpu_burn -s $((S - 1)) -p 9 > "log_burn_$L.txt"
fi
wait
cat log_sim_$L.txt log_ctrl_$L.txt
[ "$LOAD" = "carga" ] && cat log_burn_$L.txt
true
