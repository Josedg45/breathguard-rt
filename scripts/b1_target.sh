#!/bin/sh
# b1_target.sh - corre en la VM QNX: el ventilador completo (sin Pulse, pulmón RC fijo) con HMI, y a los 20 s una
# HMI comprometida (evil_hmi) ataca la planta durante 2 s. Prueba de la tarea 4 (autorización por pid).
# uso: b1_target.sh etiqueta segundos ip_host
cd "$(dirname "$0")" || exit 1
# El ventilador que arranca con la imagen (punto C) ocupa los nombres: se detiene antes de la prueba.
[ -x /system/bg/bg_stop.sh ] && /system/bg/bg_stop.sh
L=$1; S=$2; HOST=$3
rm -f log_*_$L.txt
./pneumatic_sim -s $((S + 2)) -e 5 -o "sim_$L.csv" > "log_sim_$L.txt" &
sleep 1
./safety_mon -s "$S" -H "$HOST" -o "ev_$L.csv" > "log_safety_$L.txt" 2>&1 &
sleep 20
./evil_hmi -d 2 > "log_evil_$L.txt" 2>&1
echo "evil_hmi salió con $?" >> "log_evil_$L.txt"
wait
cat log_sim_$L.txt log_safety_$L.txt log_evil_$L.txt
true
