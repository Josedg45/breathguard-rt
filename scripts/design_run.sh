#!/bin/bash
# design_run.sh - corridas de referencia en QNX para design/ (modelo de Simulink del usuario):
#   1. PCV 20/5, 12 rpm, I:E 1:2, 60 s: pneumatic_sim + breath_ctrl solos (sin Pulse: R y C fijos)
#   2. Escalones en lazo abierto: pneumatic_sim + step_test, 16 s
# Luego genera la referencia ideal en el host (design_ref) y exporta todo con scripts/design_export.py.
# uso: scripts/design_run.sh   (la VM en 172.31.1.2)
set -euo pipefail
cd "$(dirname "$0")/.."
VM=root@172.31.1.2; DIR=/tmp/medpower; OUT=results/design
SSH="ssh -o LogLevel=ERROR"; SCP="scp -o LogLevel=ERROR -q"
BINS="pneumatic_sim breath_ctrl step_test"
make -s $(for b in $BINS; do echo build/$b; done) build/design_ref
$SSH $VM "mkdir -p $DIR"
for b in $BINS; do
  $SCP build/$b $VM:$DIR/$b.new
  $SSH $VM "mv -f $DIR/$b.new $DIR/$b && chmod +x $DIR/$b"
done
mkdir -p $OUT

echo "design_run: PCV 60 s en QNX"
$SSH $VM "cd $DIR && (./pneumatic_sim -s 63 -e 5 -o sim_dref.csv > log_sim_dref.txt &) && sleep 0.5 && \
  ./breath_ctrl -s 60 -m pcv -P 20 -e 5 -r 12 -i 2 -o ctrl_dref.csv && sleep 3 && cat log_sim_dref.txt"
echo "design_run: escalones en QNX"
$SSH $VM "cd $DIR && (./pneumatic_sim -s 17 -e 5 -o sim_dstep.csv > log_sim_dstep.txt &) && sleep 0.5 && \
  ./step_test -o step_dstep.csv && sleep 1.5 && cat log_sim_dstep.txt"
for f in sim_dref ctrl_dref sim_dstep step_dstep; do $SCP $VM:$DIR/$f.csv $OUT/$f.csv; done
$SSH $VM "rm -f $DIR/sim_dref.csv $DIR/ctrl_dref.csv $DIR/sim_dstep.csv $DIR/step_dstep.csv $DIR/log_sim_dref.txt \
  $DIR/log_sim_dstep.txt; sync"

build/design_ref pcv $OUT/ref_pcv_offline.csv
build/design_ref step $OUT/step_offline.csv
python3 scripts/design_export.py
