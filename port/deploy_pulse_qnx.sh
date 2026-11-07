#!/bin/bash
# Copia a la VM QNX pulse_probe y los datos de Pulse que necesita para correr.
set -euo pipefail

QNX_SSH=root@172.31.1.2
DEST=/data/pulse
SSH_OPTS=(-o LogLevel=ERROR -o ConnectTimeout=5 -o BatchMode=yes)
REPO="$(cd "$(dirname "$0")/.." && pwd)"
DATA="$HOME/pulse/build/install/bin"

ssh "${SSH_OPTS[@]}" "$QNX_SSH" "mkdir -p $DEST/test_results"
scp "${SSH_OPTS[@]}" -q "$REPO/build/pulse_probe_qnx" "$QNX_SSH:$DEST/pulse_probe"
(cd "$DATA" && tar cf - config ecg environments nutrition patients resource states substances) |
    ssh "${SSH_OPTS[@]}" "$QNX_SSH" "cd $DEST && tar xf -"
ssh "${SSH_OPTS[@]}" "$QNX_SSH" "chmod +x $DEST/pulse_probe; ls $DEST; df -k /data | tail -1"
