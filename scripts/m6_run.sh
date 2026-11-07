#!/bin/bash
# m6_run.sh - desde Ubuntu: compila, copia, recibe la telemetría UDP y corre una escena de la HMI.
# uso: scripts/m6_run.sh etiqueta segundos escena
set -euo pipefail
cd "$(dirname "$0")/.."
L=$1; S=$2; ESC=$3
VM=root@172.31.1.2; HOST=172.31.1.1; DIR=/tmp/medpower; SSH="ssh -o LogLevel=ERROR"; SCP="scp -o LogLevel=ERROR -q"
BINS="pneumatic_sim breath_ctrl safety_mon bg_fault power_mgr hmi patient"
make $(for b in $BINS; do echo build/$b; done)
$SSH $VM "mkdir -p $DIR"; mkdir -p results/m6
for b in $BINS; do
  $SCP build/$b $VM:$DIR/$b.new
  $SSH $VM "mv -f $DIR/$b.new $DIR/$b && chmod +x $DIR/$b"
done
$SCP scripts/m6_target.sh $VM:$DIR/
python3 scripts/hmi_recv.py results/m6/hmi_$L.csv $((S + 8)) &
RECV=$!
$SSH $VM "sh $DIR/m6_target.sh $L $S $ESC $HOST" | tee results/m6/log_$L.txt
wait $RECV
$SCP $VM:$DIR/sim_$L.csv $VM:$DIR/pat_$L.csv $VM:$DIR/ev_$L.csv results/m6/
$SSH $VM "rm -f $DIR/*_$L.csv $DIR/log_*_$L.txt; sync"
