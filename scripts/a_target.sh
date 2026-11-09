#!/bin/sh
# a_target.sh - corre en la VM QNX: ventilador (sin Pulse) + power_mgr + ventd, y prueba /dev/ventilator.
#   0-10 s   línea base
#   10-20 s  un cliente lee /dev/ventilator/status sin parar (cat en bucle, prio 10)
#   20-25 s  ventd congelado (SIGSTOP); un lector queda bloqueado; después SIGCONT
#   25-40 s  escrituras: aceptada, fuera de rango, mal formada, sobre un archivo de solo lectura
# uso: a_target.sh etiqueta
cd "$(dirname "$0")" || exit 1
L=$1; S=40
rm -f log_*_$L.txt
./pneumatic_sim -s $((S + 2)) -e 5 -o "sim_$L.csv" > "log_sim_$L.txt" &
sleep 1
./safety_mon -s "$S" -o "ev_$L.csv" > "log_safety_$L.txt" 2>&1 &
./power_mgr -s "$S" > "log_pwr_$L.txt" 2>&1 &
./ventd -s $((S - 1)) > "log_ventd_$L.txt" 2>&1 &
T0=$(date +%s)
sleep 10
{
echo "== ls -l /dev/ventilator"; ls -l /dev/ventilator
echo "== status"; cat /dev/ventilator/status
n=0; end=$(( $(date +%s) + 10 ))
echo "MARCA lectura_inicio $(date +%s)"
while [ $(date +%s) -lt $end ]; do cat /dev/ventilator/status > /dev/null; n=$((n + 1)); done
echo "MARCA lectura_fin $(date +%s)"
echo "LECTURAS $n"
slay -s STOP ventd > /dev/null
echo "MARCA congelado $(date +%s)"
( cat /dev/ventilator/alarms > "cat_bloq_$L.txt" 2>&1; echo "CAT_TERMINO $(date +%s)" >> "cat_bloq_$L.txt" ) &
sleep 5
echo "MARCA descongelado $(date +%s)"
slay -s CONT ventd > /dev/null
sleep 1
cat "cat_bloq_$L.txt"
echo "== write p_insp=22"; echo "p_insp=22" > /dev/ventilator/settings; echo "RC_OK $?"
sleep 1
echo "== write p_insp=50"; echo "p_insp=50" > /dev/ventilator/settings; echo "RC_RANGO $?"
echo "== write foo"; echo "foo" > /dev/ventilator/settings; echo "RC_MAL $?"
echo "== write status"; echo "p_insp=10" > /dev/ventilator/status; echo "RC_RO $?"
sleep 7
echo "== settings"; cat /dev/ventilator/settings
echo "T0 $T0"
} > "log_escena_$L.txt" 2>&1
wait
cat log_escena_$L.txt log_sim_$L.txt log_safety_$L.txt log_ventd_$L.txt
rm -f "cat_bloq_$L.txt"
true
