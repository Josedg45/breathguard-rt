#!/bin/sh
# M1: corre en la VM QNX. Lo lanza scripts/m1_run.sh desde el host.
BIN=/tmp/medpower
OUT=/tmp/medpower/results_m1
REPS=3
SECS=10

rm -rf "$OUT"
mkdir -p "$OUT"
cd "$BIN" || exit 1
# El ventilador que arranca con la imagen (punto C) ocupa CPU y nombres: se detiene antes de la prueba.
[ -x /system/bg/bg_stop.sh ] && /system/bg/bg_stop.sh
SUM="$OUT/summary.csv"
NOISE="$OUT/noise_summary.csv"

run() {   # etiqueta, opciones extra de latency_test
    label=$1; shift
    ./latency_test -s $SECS -l "$label" -c "$OUT/$label.csv" -S "$SUM" "$@"
}

i=1
while [ $i -le $REPS ]; do
    echo "=== repeticion $i/$REPS ==="

    # Piso de ruido de la VM: nada de QNX puede desplazar a este hilo.
    ./vm_noise -s $SECS -c 1 -l "noise_idle_$i" -S "$NOISE"
    ./cpu_burn -s $((SECS + 2)) -n 1 > /dev/null &
    sleep 1
    ./vm_noise -s $SECS -c 1 -l "noise_load_$i" -S "$NOISE"
    wait

    # Reposo
    run "idle_$i"
    run "idle_pin1_$i" -C 1

    # CPU saturada: un burner por vCPU a prio 9, lazo a prio 50.
    ./cpu_burn -s $((SECS + 2)) > "$OUT/burn_load_$i.txt" &
    sleep 1
    run "load_$i"
    wait
    cat "$OUT/burn_load_$i.txt"

    ./cpu_burn -s $((SECS + 2)) > "$OUT/burn_load_pin1_$i.txt" &
    sleep 1
    run "load_pin1_$i" -C 1
    wait
    cat "$OUT/burn_load_pin1_$i.txt"

    i=$((i + 1))
done

# Control: el mismo lazo POR DEBAJO de la carga (prio 5 < 9). Se lanza primero
# para que arme el timer antes de quedarse sin CPU. Debe degradarse muchísimo;
# si no, la carga no estaba compitiendo y las medidas anteriores no probarían nada.
echo "=== control: lazo por debajo de la carga ==="
run "control_prio5" -p 5 &
sleep 1
./cpu_burn -s 6 > "$OUT/burn_control.txt"
wait
cat "$OUT/burn_control.txt"
