#!/bin/bash
# M1: compila, despliega, mide en la VM QNX y trae los CSV a results/m1/.
set -euo pipefail

QNX=root@172.31.1.2
SSH_OPTS=(-o LogLevel=ERROR -o ConnectTimeout=5 -o BatchMode=yes)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if ! ssh "${SSH_OPTS[@]}" "$QNX" true; then
    echo "La VM no responde por SSH. Revisa que esté encendida y repón la IP:" >&2
    echo "  ifconfig vtnet0 172.31.1.2 netmask 255.255.255.0" >&2
    exit 1
fi

make deploy
scp "${SSH_OPTS[@]}" -q scripts/m1_target.sh "$QNX":/tmp/medpower/
ssh "${SSH_OPTS[@]}" "$QNX" "sh /tmp/medpower/m1_target.sh"

rm -rf results/m1
mkdir -p results/m1
scp "${SSH_OPTS[@]}" -q -r "$QNX":/tmp/medpower/results_m1/. results/m1/

python3 scripts/m1_check.py results/m1
