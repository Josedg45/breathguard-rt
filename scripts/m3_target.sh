#!/bin/sh
# m3_target.sh - corre en la VM QNX: pneumatic_sim + breath_ctrl con unos ajustes de PCV.
# uso: m3_target.sh etiqueta p_insp peep rr segundos
cd "$(dirname "$0")" || exit 1
L=$1; P=$2; E=$3; R=$4; S=$5
./pneumatic_sim -s $((S + 2)) -e "$E" -o "sim_$L.csv" &
sleep 1   # la planta arranca primero y publica su nombre
./breath_ctrl -s "$S" -P "$P" -e "$E" -r "$R" -o "ctrl_$L.csv"
wait
