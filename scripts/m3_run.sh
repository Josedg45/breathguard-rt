#!/bin/bash
# m3_run.sh - desde Ubuntu: compila, copia a la VM, corre PCV y trae los CSV a results/m3/.
# uso: scripts/m3_run.sh etiqueta p_insp peep rr [segundos] [carga] [pulse]
set -euo pipefail
cd "$(dirname "$0")/.."
L=$1; P=$2; E=$3; R=$4; S=${5:-30}; shift 4; shift || true; OPTS="$*"
VM=root@172.31.1.2; DIR=/tmp/medpower; SSH="ssh -o LogLevel=ERROR"; SCP="scp -o LogLevel=ERROR -q"
BINS="pneumatic_sim breath_ctrl cpu_burn"
case " $OPTS " in *" pulse "*) BINS="$BINS patient";; esac
make $(for b in $BINS; do echo build/$b; done)
$SSH $VM "mkdir -p $DIR"; mkdir -p results/m3
# Nombre temporal + mv: scp no puede sobrescribir un binario en ejecución (ver CLAUDE.md).
for b in $BINS; do
  $SCP build/$b $VM:$DIR/$b.new
  $SSH $VM "mv -f $DIR/$b.new $DIR/$b && chmod +x $DIR/$b"
done
$SCP scripts/m3_target.sh $VM:$DIR/
$SSH $VM "sh $DIR/m3_target.sh $L $P $E $R $S $OPTS" | tee results/m3/log_$L.txt

$SCP $VM:$DIR/sim_$L.csv $VM:$DIR/ctrl_$L.csv results/m3/
case " $OPTS " in *" pulse "*) $SCP $VM:$DIR/pat_$L.csv results/m3/;; esac
# /tmp de la VM está en /data (62 MB, compartido con Pulse): no dejar CSV allí.
$SSH $VM "rm -f $DIR/*_$L.csv $DIR/log_*_$L.txt; sync"
python3 scripts/m3_analyze.py results/m3/sim_$L.csv $P $E $R --ctrl results/m3/ctrl_$L.csv \
  --plot results/m3/$L.png
