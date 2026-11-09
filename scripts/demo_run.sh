#!/bin/bash
# demo_run.sh - la demo del video: el sistema completo en QNX y, desde el host, las escenas en momentos fijos con
# un subtitulo en el dashboard para cada una. Todo queda grabado en results/demo/ para repetirlo con --replay.
# uso: scripts/demo_run.sh [--no-gui] [segundos]   (180 por defecto; la VM en 172.31.1.2)
#
# Linea de tiempo (s desde el arranque):
#   5 PCV estable | 22 matar la HMI | 38 CPU saturada 20 s | 62 tubo desconectado 12 s | 88 kill -9 breath_ctrl
#   108 comando del operador p_insp 25 | 125 corte de red | 160 vuelve la red
set -euo pipefail
cd "$(dirname "$0")/.."
GUI=1
if [ "${1:-}" = "--no-gui" ]; then GUI=0; shift; fi
S=${1:-180}
VM=root@172.31.1.2; HOST=172.31.1.1; DIR=/tmp/medpower; L=demo
SSH="ssh -o LogLevel=ERROR"; SCP="scp -o LogLevel=ERROR -q"
BINS="pneumatic_sim breath_ctrl safety_mon bg_fault power_mgr hmi cpu_burn patient"
make -s $(for b in $BINS; do echo build/$b; done)
$SSH $VM "mkdir -p $DIR"
for b in $BINS; do
  $SCP build/$b $VM:$DIR/$b.new
  $SSH $VM "mv -f $DIR/$b.new $DIR/$b && chmod +x $DIR/$b"
done
$SCP scripts/demo_target.sh $VM:$DIR/
OUTD=results/demo; mkdir -p $OUTD
STAMP=$(date +%Y%m%d_%H%M%S); OUT=$OUTD/hmi_$STAMP.csv

if [ $GUI = 1 ]; then
  python3 scripts/dashboard.py --record "$OUT" &
else
  python3 scripts/hmi_recv.py "$OUT" $((S + 8)) &
fi
REC=$!
sleep 2

cap() {  # subtitulo para el dashboard (y para el registro)
  python3 -c 'import socket,sys; socket.socket(socket.AF_INET,socket.SOCK_DGRAM).sendto(("CAPTION,"+sys.argv[1]).encode(),("127.0.0.1",5005))' "$1"
  echo "$(date +%T.%N | cut -c1-12) $1" | tee -a $OUTD/captions_$STAMP.txt
}
vm() { $SSH $VM "cd $DIR && $*" < /dev/null || true; }  # slay sale con el número de procesos (no es error)
T0=$(date +%s%N)
at() { while [ $(date +%s%N) -lt $((T0 + $1 * 1000000000)) ]; do sleep 0.05; done; }

$SSH $VM "sh $DIR/demo_target.sh $L $S $HOST" > $OUTD/log_$STAMP.txt 2>&1 &
TGT=$!
at 5;   cap "1/6  Stable ventilation: PCV 20/5 cmH2O, 12 bpm. Virtual patient (Pulse) runs inside QNX 8.0"
at 22;  cap "2/6  Killing the display process (HMI)... the patient keeps breathing; the HMI is relaunched"
        vm "slay -s KILL hmi"
at 38;  cap "3/6  Both CPUs saturated with junk load for 20 s: the breathing loop keeps its period"
        vm "./cpu_burn -s 20 -p 9 > /dev/null 2>&1 &"
at 62;  cap "4/6  Tube disconnected: alarm in ~20 ms"
        vm "./bg_fault -d 1 > /dev/null"
at 74;  vm "./bg_fault > /dev/null"
        cap "     Tube reconnected"
at 88;  cap "5/6  kill -9 breath_ctrl: safety_mon takes over in ~2 ms and relaunches it"
        vm "slay -s KILL breath_ctrl"
at 108; cap "     Operator command: Pinsp 20 -> 25 cmH2O (validated by safety_mon, applied at next breath)"
        (python3 scripts/bg_cmd.py p_insp=25 || true) | tee -a $OUTD/captions_$STAMP.txt
at 125; cap "6/6  Mains power lost: on battery, non-essential loads shed, autonomy estimate (battery time x60)"
        vm "./bg_fault -m 0 > /dev/null"
at 160; vm "./bg_fault -m 1 > /dev/null"
        cap "     Mains restored"
wait $TGT
$SCP $VM:$DIR/sim_$L.csv $OUTD/sim_$STAMP.csv
$SCP $VM:$DIR/pat_$L.csv $OUTD/pat_$STAMP.csv
$SCP $VM:$DIR/ev_$L.csv $OUTD/ev_$STAMP.csv
$SCP $VM:$DIR/pwr_$L.csv $OUTD/pwr_$STAMP.csv
$SSH $VM "rm -f $DIR/*_$L.csv $DIR/log_*_$L.txt; sync"
if [ $GUI = 1 ]; then echo "demo: listo; cierra la ventana del dashboard. Grabado en $OUT"; else wait $REC; fi
grep "safety_mon: t=" $OUTD/log_$STAMP.txt || true
