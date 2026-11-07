#!/bin/bash
# m5_run.sh - desde Ubuntu: compila, copia a la VM y corre la escena de energía (ver m5_target.sh).
# uso: scripts/m5_run.sh etiqueta [segundos] [escala] [soc_inicial] [t_corte] [duración_corte]
set -euo pipefail
cd "$(dirname "$0")/.."
L=$1; S=${2:-90}; X=${3:-60}; B=${4:-1.0}; TC=${5:-30}; DC=${6:-45}
VM=root@172.31.1.2; DIR=/tmp/medpower; SSH="ssh -o LogLevel=ERROR"; SCP="scp -o LogLevel=ERROR -q"
BINS="pneumatic_sim breath_ctrl safety_mon bg_fault power_mgr patient"
make $(for b in $BINS; do echo build/$b; done)
$SSH $VM "mkdir -p $DIR"; mkdir -p results/m5
for b in $BINS; do
  $SCP build/$b $VM:$DIR/$b.new
  $SSH $VM "mv -f $DIR/$b.new $DIR/$b && chmod +x $DIR/$b"
done
$SCP scripts/m5_target.sh $VM:$DIR/
$SSH $VM "sh $DIR/m5_target.sh $L $S $X $B $TC $DC" | tee results/m5/log_$L.txt
$SCP $VM:$DIR/sim_$L.csv $VM:$DIR/pat_$L.csv $VM:$DIR/ev_$L.csv $VM:$DIR/pwr_$L.csv results/m5/
$SSH $VM "rm -f $DIR/*_$L.csv $DIR/log_*_$L.txt; sync"
