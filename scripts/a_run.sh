#!/bin/bash
# a_run.sh - desde Ubuntu: compila, copia y corre la prueba de /dev/ventilator (ventd) en la VM QNX; la evalúa
# con a_check.py. uso: scripts/a_run.sh [etiqueta]
set -euo pipefail
cd "$(dirname "$0")/.."
L=${1:-ventd}
VM=root@172.31.1.2; DIR=/tmp/medpower; SSH="ssh -o LogLevel=ERROR"; SCP="scp -o LogLevel=ERROR -q"
BINS="pneumatic_sim breath_ctrl safety_mon power_mgr ventd"
make -s $(for b in $BINS; do echo build/$b; done)
$SSH $VM "mkdir -p $DIR"; mkdir -p results/ventd
for b in $BINS; do
  $SCP build/$b $VM:$DIR/$b.new
  $SSH $VM "mv -f $DIR/$b.new $DIR/$b && chmod +x $DIR/$b"
done
$SCP scripts/a_target.sh $VM:$DIR/
$SSH $VM "sh $DIR/a_target.sh $L" > results/ventd/log_$L.txt
$SCP $VM:$DIR/sim_$L.csv $VM:$DIR/ev_$L.csv results/ventd/
$SSH $VM "rm -f $DIR/*_$L.csv $DIR/log_*_$L.txt; sync"
python3 scripts/a_check.py results/ventd "$L"
