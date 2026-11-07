#!/bin/bash
# m3_run.sh - desde Ubuntu: compila, copia a la VM, corre PCV y trae los CSV a results/m3/.
# uso: scripts/m3_run.sh etiqueta p_insp peep rr [segundos]
set -euo pipefail
cd "$(dirname "$0")/.."
L=$1; P=$2; E=$3; R=$4; S=${5:-30}
VM=root@172.31.1.2; DIR=/tmp/medpower; SSH="ssh -o LogLevel=ERROR"; SCP="scp -o LogLevel=ERROR -q"
make m3
$SSH $VM "mkdir -p $DIR"
# Nombre temporal + mv: scp no puede sobrescribir un binario en ejecución (ver CLAUDE.md).
for b in pneumatic_sim breath_ctrl; do
  $SCP build/$b $VM:$DIR/$b.new
  $SSH $VM "mv -f $DIR/$b.new $DIR/$b && chmod +x $DIR/$b"
done
$SCP scripts/m3_target.sh $VM:$DIR/
$SSH $VM "sh $DIR/m3_target.sh $L $P $E $R $S"
mkdir -p results/m3
$SCP $VM:$DIR/sim_$L.csv $VM:$DIR/ctrl_$L.csv results/m3/
python3 scripts/m3_analyze.py results/m3/sim_$L.csv $P $E $R --ctrl results/m3/ctrl_$L.csv \
  --plot results/m3/$L.png
