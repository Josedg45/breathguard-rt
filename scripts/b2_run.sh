#!/bin/bash
# b2_run.sh - from Ubuntu: layer-2 test (secpol policy) on the QNX VM built by scripts/image_build.sh.
# Uses the binaries baked into the image (/system/bg), not /tmp. Usage: scripts/b2_run.sh [label]
set -euo pipefail
cd "$(dirname "$0")/.."
L=${1:-b2}
VM=root@172.31.1.2; SSH="ssh -o LogLevel=ERROR"; SCP="scp -o LogLevel=ERROR -q"
mkdir -p results/sec
$SCP scripts/b2_target.sh $VM:/tmp/b2_target.sh
$SSH $VM "sh /tmp/b2_target.sh" > results/sec/log_$L.txt 2>&1
python3 scripts/b2_check.py results/sec/log_$L.txt
