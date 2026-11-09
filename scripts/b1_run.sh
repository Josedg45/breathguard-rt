#!/bin/bash
# b1_run.sh - desde Ubuntu: compila, copia y corre la prueba de la tarea 4 (HMI comprometida) en la VM QNX; luego
# la evalúa con b1_check.py. uso: scripts/b1_run.sh [etiqueta] [segundos]
set -euo pipefail
cd "$(dirname "$0")/.."
L=${1:-b1}; S=${2:-40}
VM=root@172.31.1.2; HOST=172.31.1.1; DIR=/tmp/medpower; SSH="ssh -o LogLevel=ERROR"; SCP="scp -o LogLevel=ERROR -q"
BINS="pneumatic_sim breath_ctrl safety_mon hmi evil_hmi"
make -s $(for b in $BINS; do echo build/$b; done)
$SSH $VM "mkdir -p $DIR"; mkdir -p results/sec
for b in $BINS; do
  $SCP build/$b $VM:$DIR/$b.new
  $SSH $VM "mv -f $DIR/$b.new $DIR/$b && chmod +x $DIR/$b"
done
$SCP scripts/b1_target.sh $VM:$DIR/
python3 scripts/hmi_recv.py results/sec/hmi_$L.csv $((S + 4)) &
RECV=$!
$SSH $VM "sh $DIR/b1_target.sh $L $S $HOST" > results/sec/log_$L.txt
wait $RECV
$SCP $VM:$DIR/sim_$L.csv $VM:$DIR/ev_$L.csv results/sec/
$SSH $VM "rm -f $DIR/*_$L.csv $DIR/log_*_$L.txt; sync"
python3 scripts/b1_check.py results/sec "$L"
