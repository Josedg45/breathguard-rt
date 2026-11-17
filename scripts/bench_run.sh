#!/bin/bash
# bench_run.sh - the interactive test bench in one command: deploy, start the ventilator with a Pulse patient on the
# QNX VM, open the dashboard with its control panel; closing the dashboard stops the bench.
# use: scripts/bench_run.sh [patient_id]      (scripts/bench.py list shows the patients)
#   NO_DEPLOY=1  skip build and copy (binaries and patient states already on the VM)
# Environment as in scripts/bench.py: BG_VM, BG_SSH, BG_HOST.
set -euo pipefail
cd "$(dirname "$0")/.."
PATIENT=${1:-healthy_male}
if [ "${NO_DEPLOY:-0}" != 1 ]; then
    python3 scripts/bench.py deploy
fi
python3 scripts/bench.py start "$PATIENT"
trap 'python3 scripts/bench.py stop || true' EXIT
python3 scripts/dashboard.py --control "${@:2}"
