#!/usr/bin/env bash
# (Re)starts the test VM and gets it to a usable state: "Try air/OS", sshd
# started from a Terminal, the data disk mounted at /Data (formatted on its
# first use). Positions are for the 1280x800 screen and 16 CPUs of run-vm.sh.
set -euo pipefail
HERE=$(dirname "$(readlink -f "$0")")
. "$HERE/env.sh"
if [[ -f $VM_DIR/qemu.pid ]] && kill -0 "$(cat "$VM_DIR/qemu.pid")" 2>/dev/null; then
	kill "$(cat "$VM_DIR/qemu.pid")"; sleep 2
fi
"$HERE/run-vm.sh"
q() { python3 "$HERE/qmp.py" "$@" >/dev/null; }
for i in $(seq 60); do sleep 2; q shot "$VM_DIR/boot.png" 2>/dev/null && break; done
sleep 25
q click 902 582		# Try air/OS
sleep 15
# sshd from a Terminal: ProcessController's "New Terminal"
q click 1168 36
sleep 2
q click 1140 471
sleep 10
q type 'ssh-keygen -A; /bin/sshd
'
for i in $(seq 40); do sleep 3; "$HERE/ssh.sh" true 2>/dev/null && break; done
"$HERE/ssh.sh" 'mkdir -p /Data
if ! mount /dev/disk/ata/0/slave/raw /Data 2>/dev/null; then
	mkfs -q -t bfs /dev/disk/ata/0/slave/raw Data >/dev/null && mount /dev/disk/ata/0/slave/raw /Data
fi
df -h /Data | grep -c Data' && echo "VM $VM_NAME up: ssh port $VM_SSH_PORT, VNC :$VM_VNC_DISPLAY"
