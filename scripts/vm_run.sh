#!/bin/bash
# vm_run.sh - arranca la VM QNX sin terminal propia: la consola serie queda en un archivo. Mide, desde el host,
# cuánto tarda en responder al ping y en llegar el primer paquete de telemetría de la HMI.
# uso: scripts/vm_run.sh [log_consola]   (detenerla: kill $(cat ~/qnxvm/output/qemu.pid))
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
VMDIR=${VMDIR:-$HOME/qnxvm}
LOG=$(realpath -m "${1:-$REPO/results/boot/console_$(date +%Y%m%d_%H%M%S).txt}")
mkdir -p "$(dirname "$LOG")"
source ~/qnx800/qnxsdp-env.sh > /dev/null
cd "$VMDIR"
# Con IP fija no hace falta el DHCP (dnsmasq) del modo "nat": basta con el puente br0.
export OPT_NETWORK=${OPT_NETWORK:-bridge:br0}
T0=$(date +%s.%N)
# script da a QEMU una pseudo-terminal (usa la consola serie por stdio) y la registra con marcas de tiempo.
# setsid: QEMU sigue corriendo aunque se cierre esta terminal.
setsid bash -c 'T0=$1; script -qfc "mkqnximage --run" /dev/null < /dev/null | while IFS= read -r line; do
    printf "%.3f %s\n" "$(echo "$(date +%s.%N) - $T0" | bc)" "$line"
done' _ "$T0" > "$LOG" 2>&1 &
echo "vm_run: QEMU lanzado; consola en $LOG"
python3 "$REPO/scripts/boot_watch.py" "$T0" "$LOG"
