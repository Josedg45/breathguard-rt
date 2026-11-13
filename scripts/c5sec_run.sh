#!/bin/bash
# c5sec_run.sh - from Ubuntu: layer-2 (secpol) test of the copilot on the VM image built by scripts/image_build.sh.
# Needs the image with the copilot type (the binaries come from /system/bg, not from this build).
# use: scripts/c5sec_run.sh [label]
set -euo pipefail
cd "$(dirname "$0")/.."
L=${1:-c5sec}
VM=root@172.31.1.2; SSH="ssh -o LogLevel=ERROR"; SCP="scp -o LogLevel=ERROR -q"
mkdir -p results/copilot
$SSH $VM "mkdir -p /tmp/medpower"
$SCP scripts/c5sec_target.sh $VM:/tmp/medpower/
$SSH $VM "sh /tmp/medpower/c5sec_target.sh" > results/copilot/log_$L.txt 2>&1
LOG=results/copilot/log_$L.txt
fails=0
check() { # name, grep pattern (extended), expected count: "some" (>= 1) or "none" (0)
  local n
  n=$(grep -c -E "$2" "$LOG" || true)
  if { [ "$3" = some ] && [ "$n" -gt 0 ]; } || { [ "$3" = none ] && [ "$n" -eq 0 ]; }; then
    echo "  [OK] $1 ($n)"
  else
    echo "  [FAIL] $1 ($n)"; fails=$((fails + 1))
  fi
}
echo "Copilot layer 2 ($L):"
check "the liar runs with the copilot type" "liar_copilot +bg_safety_t__cop" some
check "kernel refuses the liar name_open(plant)" "liar_copilot: name_open\(plant\) = -1 \(Operation not permitted\)" some
check "kernel refuses the liar open(/dev/ventilator/copilot)" "liar_copilot: open\(/dev/ventilator/copilot\) = -1 \(Operation not permitted\)" some
check "a shell with the copilot type cannot write accept" "^RC_COPILOT_TYPE_WRITE [1-9]" some
check "root can read /dev/ventilator/copilot" "^RC_ROOT_READ 0" some
check "safety_mon refuses the liar's direct self-accept (layer 1, pid)" "COPILOTO_AUTOCONFIRMA" some
check "safety_mon blocks its proposals (fixed rules)" "COPILOTO_BLOQUEADO" some
check "no proposal ever reaches the operator" "COPILOTO_PROPONE" none
check "no controller failure" "CONTROL_MUERTO|CONTROL_COLGADO|APNEA_CON_CONTROL_VIVO" none
check "boot stack running again with the real copilot" "\./copilot +copilot +bg_safety_t__cop" some
echo "  [info] kernel denials logged (all expected to come from the liar or the copilot-typed shell):"
sed -n '/== secpolgenerate errors/,/== safety_mon log/p' "$LOG" | grep -v "^==" | sed 's/^/      /' | head -20
echo "COPILOT LAYER 2 $L: $([ $fails = 0 ] && echo PASS || echo FAIL)"
[ $fails = 0 ]
