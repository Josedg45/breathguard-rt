#!/bin/sh
# M2: corre en la VM QNX. Mide el paso de tiempo de Pulse en QNX, con y sin carga.
cd /data/pulse || exit 1
# El ventilador que arranca con la imagen (punto C) ocupa CPU y nombres: se detiene antes de la prueba.
[ -x /system/bg/bg_stop.sh ] && /system/bg/bg_stop.sh
OUT=/data/pulse/results_m2
rm -rf "$OUT"
mkdir -p "$OUT"
LOG="$OUT/qnx_step_times.txt"
: > "$LOG"

run() {   # etiqueta, argumentos de pulse_probe
    label=$1; shift
    echo "=== $label" | tee -a "$LOG"
    ./pulse_probe "$@" | tee -a "$LOG"
}

for i in 1 2 3; do
    run "external_reposo_$i" external 180 "$OUT/qnx_external_pcv20_5_12_$i.csv"
done
run internal internal 180 "$OUT/qnx_internal_pcv20_5_12.csv"
run external_30_10_20 external 120 "$OUT/qnx_external_pcv30_10_20.csv" 30 10 20 0.5
run external_12_0_10 external 120 "$OUT/qnx_external_pcv12_0_10.csv" 12 0 10 0.25

# CPU saturada: Pulse a su prioridad normal (10) por encima de la carga (9). La carga dura
# de sobra para cubrir toda la corrida aunque Pulse se vuelva más lento.
for i in 1 2 3; do
    /tmp/medpower/cpu_burn -s 75 > "$OUT/burn_$i.txt" &
    sleep 1
    run "external_carga_$i" external 180 "$OUT/qnx_external_carga_$i.csv"
    wait
    cat "$OUT/burn_$i.txt" | tee -a "$LOG"
done
