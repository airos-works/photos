#!/usr/bin/env bash
# Starts a Photos test VM: the air/OS x86_64 image through a fresh qcow2
# overlay (the image boots as a live system: "Try air/OS"), plus a data disk
# that keeps its contents between boots (mounted at /Data by vm-up.sh).
# QEMU_EXTRA adds devices, e.g. a phone:
#   QEMU_EXTRA="-device usb-mtp,rootdir=/path/to/pictures,readonly=on"
set -euo pipefail
. "$(dirname "$(readlink -f "$0")")/env.sh"
mkdir -p "$VM_DIR"
if [[ -f $VM_DIR/qemu.pid ]] && kill -0 "$(cat "$VM_DIR/qemu.pid")" 2>/dev/null; then
	echo "already running"; exit 0
fi
[[ -f $VM_BASE/airos.img ]] || { echo "no base image in $VM_BASE (tools/vm/README.md)" >&2; exit 1; }
rm -f "$VM_DIR/overlay.qcow2" "$VM_DIR/serial.log"
qemu-img create -q -f qcow2 -b "$VM_BASE/airos.img" -F raw "$VM_DIR/overlay.qcow2" 12G
[[ -f $VM_DIR/data.qcow2 ]] || qemu-img create -q -f qcow2 "$VM_DIR/data.qcow2" 40G
# shellcheck disable=SC2086
exec qemu-system-x86_64 -name "photos-$VM_NAME" -enable-kvm -cpu host -smp 16 -m ${VM_MEM:-8G} \
  -drive file="$VM_DIR/overlay.qcow2",format=qcow2,if=ide,index=0 \
  -drive file="$VM_DIR/data.qcow2",format=qcow2,if=ide,index=1 \
  -nic user,model=e1000,hostfwd=tcp:127.0.0.1:$VM_SSH_PORT-:22 \
  -device qemu-xhci,id=xhci -device usb-tablet -vga std -display none -vnc 127.0.0.1:$VM_VNC_DISPLAY \
  -audiodev none,id=snd -device intel-hda -device hda-duplex,audiodev=snd \
  -qmp unix:"$VM_DIR/qmp.sock",server=on,wait=off -pidfile "$VM_DIR/qemu.pid" \
  -serial file:"$VM_DIR/serial.log" -daemonize ${QEMU_EXTRA:-}
