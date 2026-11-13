#!/bin/bash
# image_build.sh - genera la imagen QNX de la VM con el ventilador dentro y su arranque automático (punto C).
# Copia image/*.custom a ~/qnxvm/local/snippets y llama a mkqnximage. NO arranca la VM (ver scripts/vm_run.sh).
# uso: scripts/image_build.sh [opciones extra de mkqnximage]
# Por defecto: --secpol=open con image/policy.txt; image/data_files.custom restringe nuestros tipos (capa 2).
# Ojo: regenerar la partición /data borra lo copiado a mano (Pulse: port/deploy_pulse_qnx.sh).
# --usb=no: la VM no tiene dispositivos USB ni el ventilador los usa. Con USB, io-sock (-m usb) reintenta abrir
# /dev/usb/io-usb-otg cada 2 s sin permiso (uid 33 frente a 0644 del uid 44) y llena la consola de errores de
# secpolgenerate. Además, menos servicios = menos superficie de ataque.
# Variables: VMDIR (por defecto ~/qnxvm), CLEAN=1 para --clean (borra /data: hay que volver a copiar Pulse).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
VMDIR=${VMDIR:-$HOME/qnxvm}
source ~/qnx800/qnxsdp-env.sh > /dev/null
make -C "$REPO" -s all
SN="$VMDIR/local/snippets"
cp "$REPO/image/startup_postfs.custom" "$SN/startup_postfs.custom"
sed "s#@BUILD@#$REPO/build#g" "$REPO/image/system_files.custom.in" > "$SN/system_files.custom"
cp "$REPO/image/data_files.custom" "$SN/data_files.custom"
cd "$VMDIR"
extra=()
[ "${CLEAN:-0}" = 1 ] && extra+=(--clean)
mkqnximage --type=qemu --arch=x86_64 --ssh-ident="$HOME/.ssh/id_ed25519.pub" --ip=172.31.1.2 \
    --data-size=200 --data-inodes=6000 --secpol=open --policy="$REPO/image/policy.txt" --usb=no "${extra[@]}" "$@"
