#!/bin/sh
# m3_target.sh - corre en la VM QNX: pneumatic_sim + breath_ctrl (+ patient con Pulse) en PCV.
# uso: m3_target.sh etiqueta p_insp peep rr segundos [carga] [pulse]
#   carga: 2 hilos basura (uno por vCPU) a prio 9, lanzados DESPUÉS del lazo (lección de M1).
#   pulse: el pulmón lo da Pulse (proceso patient, prio 20) en vez del RC fijo.
#   pulse_bajo: igual, pero patient a prio 5, POR DEBAJO de la carga (experimento de control).
cd "$(dirname "$0")" || exit 1
# El ventilador que arranca con la imagen (punto C) ocupa los nombres: se detiene antes de la prueba.
[ -x /system/bg/bg_stop.sh ] && /system/bg/bg_stop.sh
D=$(pwd)
L=$1; P=$2; E=$3; R=$4; S=$5; shift 5
LOAD=; PULSE=; PPRIO=20
for a in "$@"; do
    [ "$a" = "carga" ] && LOAD=1
    [ "$a" = "pulse" ] && PULSE=1
    [ "$a" = "pulse_bajo" ] && { PULSE=1; PPRIO=5; }
done
rm -f log_*_$L.txt
if [ -n "$PULSE" ]; then
    # patient carga Pulse (unos segundos) y luego espera a que exista la planta.
    (cd /data/pulse && "$D/patient" -p $PPRIO -s $((S + 1)) -o "$D/pat_$L.csv" > "$D/log_pat_$L.txt" 2>&1) &
    sleep 2
fi
./pneumatic_sim -s $((S + 3)) -e "$E" -o "sim_$L.csv" > "log_sim_$L.txt" &
sleep 1   # la planta arranca primero y publica su nombre
./breath_ctrl -s "$S" -P "$P" -e "$E" -r "$R" -o "ctrl_$L.csv" > "log_ctrl_$L.txt" &
if [ -n "$LOAD" ]; then
    sleep 1
    ./cpu_burn -s $((S - 1)) -p 9 > "log_burn_$L.txt"
fi
wait
cat log_*_$L.txt
true
