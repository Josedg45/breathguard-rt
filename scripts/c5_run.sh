#!/bin/bash
# c5_run.sh - from Ubuntu: build, copy and run a copilot scene in the QNX VM (c5_target.sh), bring the results to
# results/copilot/ and evaluate them with c5_check.py.
# use: scripts/c5_run.sh label rc|pulse|liar
#   COPILOT_BIN=path  use another copilot binary (e.g. one built with a finished task 5); default build/copilot.
#                     With the task 5 skeleton the tree never answers, so the copilot never suggests anything.
set -euo pipefail
cd "$(dirname "$0")/.."
L=${1:?label}; ESC=${2:?scene}
VM=root@172.31.1.2; DIR=/tmp/medpower; SSH="ssh -o LogLevel=ERROR"; SCP="scp -o LogLevel=ERROR -q"
BINS="pneumatic_sim breath_ctrl safety_mon ventd bg_fault liar_copilot"
make -s $(for b in $BINS copilot; do echo build/$b; done)
$SSH $VM "mkdir -p $DIR"; mkdir -p results/copilot
put() { # copy under another name first: a running binary cannot be overwritten (CLAUDE.md)
  $SCP "$1" $VM:$DIR/$2.new
  $SSH $VM "mv -f $DIR/$2.new $DIR/$2 && chmod +x $DIR/$2"
}
for b in $BINS; do put build/$b $b; done
put "${COPILOT_BIN:-build/copilot}" copilot
# /tmp lives in /data, which a regenerated image wipes: copy the patient too (built with: make patient)
[ "$ESC" = pulse ] && put build/patient patient
$SCP scripts/c5_target.sh $VM:$DIR/
$SSH $VM "sh $DIR/c5_target.sh $L $ESC" > results/copilot/log_$L.txt
FILES="$VM:$DIR/cop_$L.csv $VM:$DIR/ev_$L.csv $VM:$DIR/sim_$L.csv"
[ "$ESC" = pulse ] && FILES="$FILES $VM:$DIR/pat_$L.csv"
$SCP $FILES results/copilot/
$SSH $VM "rm -f $DIR/*_$L.csv $DIR/log_*_$L.txt /data/var/dumper/*.core; sync"
gzip -f -9 -n results/copilot/sim_$L.csv
[ -f results/copilot/pat_$L.csv ] && gzip -f -9 -n results/copilot/pat_$L.csv
python3 scripts/c5_check.py results/copilot "$L" "$ESC"
