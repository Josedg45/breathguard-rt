#!/bin/bash
# m6_run.sh - desde Ubuntu: compila, copia, recibe la telemetría UDP y corre una escena de la HMI.
# uso: [GUI=1] scripts/m6_run.sh etiqueta segundos escena
set -euo pipefail
cd "$(dirname "$0")/.."
L=$1; S=$2; ESC=$3
VM=root@172.31.1.2; HOST=172.31.1.1; DIR=/tmp/medpower; SSH="ssh -o LogLevel=ERROR"; SCP="scp -o LogLevel=ERROR -q"
BINS="pneumatic_sim breath_ctrl safety_mon bg_fault power_mgr hmi mono_vent patient"
make $(for b in $BINS; do echo build/$b; done)
$SSH $VM "mkdir -p $DIR"; mkdir -p results/m6
for b in $BINS; do
  $SCP build/$b $VM:$DIR/$b.new
  $SSH $VM "mv -f $DIR/$b.new $DIR/$b && chmod +x $DIR/$b"
done
$SCP scripts/m6_target.sh $VM:$DIR/
if [ "${GUI:-0}" = 1 ]; then  # GUI=1 scripts/m6_run.sh ...: dashboard en vivo (para grabar), que tambien graba
  python3 scripts/dashboard.py --record results/m6/hmi_$L.csv &
else
  python3 scripts/hmi_recv.py results/m6/hmi_$L.csv $((S + 8)) &
fi
RECV=$!
if [ "$ESC" = ajustes ]; then  # comandos del operador desde el host (t aprox. desde que arranca safety_mon)
  ( c() { sleep "$1"; shift; echo "$(date +%s.%N) $*: $(python3 scripts/bg_cmd.py "$@")"; }
    c 20 p_insp=25; c 10 p_insp=50; c 8 p_insp=25 peep=8 rr=15; c 10 foo=3; c 17 mode=vcv vt=450
  ) > results/m6/cmd_$L.txt 2>&1 &
fi
if [ "$ESC" = autopeep ]; then  # espiracion demasiado corta: auto-PEEP y su alarma; luego se corrige
  ( c() { sleep "$1"; shift; echo "$(date +%s.%N) $*: $(python3 scripts/bg_cmd.py "$@")"; }
    c 20 rr=30 ie=1; c 30 rr=12 ie=2
  ) > results/m6/cmd_$L.txt 2>&1 &
fi
$SSH $VM "sh $DIR/m6_target.sh $L $S $ESC $HOST" | tee results/m6/log_$L.txt
[ "${GUI:-0}" = 1 ] || wait $RECV
$SCP $VM:$DIR/sim_$L.csv $VM:$DIR/pat_$L.csv $VM:$DIR/ev_$L.csv results/m6/
$SSH $VM "rm -f $DIR/*_$L.csv $DIR/log_*_$L.txt; sync"
