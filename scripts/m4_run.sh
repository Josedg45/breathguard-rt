#!/bin/bash
# m4_run.sh - desde Ubuntu: compila, copia a la VM y corre una escena de seguridad (ver m4_target.sh).
# uso: scripts/m4_run.sh etiqueta segundos escena [opciones de safety_mon...]
set -euo pipefail
cd "$(dirname "$0")/.."
L=$1; S=$2; ESC=$3; shift 3
VM=root@172.31.1.2; DIR=/tmp/medpower; SSH="ssh -o LogLevel=ERROR"; SCP="scp -o LogLevel=ERROR -q"
BINS="pneumatic_sim breath_ctrl safety_mon bg_fault cpu_burn patient"
make $(for b in $BINS; do echo build/$b; done)
$SSH $VM "mkdir -p $DIR"; mkdir -p results/m4
# Nombre temporal + mv: scp no puede sobrescribir un binario en ejecución (ver CLAUDE.md).
for b in $BINS; do
  $SCP build/$b $VM:$DIR/$b.new
  $SSH $VM "mv -f $DIR/$b.new $DIR/$b && chmod +x $DIR/$b"
done
$SCP scripts/m4_target.sh $VM:$DIR/
$SSH $VM "sh $DIR/m4_target.sh $L $S $ESC $*" | tee results/m4/log_$L.txt
$SCP $VM:$DIR/sim_$L.csv $VM:$DIR/pat_$L.csv $VM:$DIR/ev_$L.csv results/m4/
# /tmp de la VM está en /data (62 MB, compartido con Pulse): no dejar CSV allí.
$SSH $VM "rm -f $DIR/*_$L.csv $DIR/log_*_$L.txt; sync"
