#!/usr/bin/env bash
# The agent by itself, in the test VM (docs/DEVICES.md): photos_agent runs,
# a stand-in for Photos (build-ARCH/fake_photos as /Data/ptp/app/Photos,
# registered with mimeset -F) logs what it is started with, and QEMU's
# usb-mtp device comes and goes.
#
#   VM_NAME=ptp tools/ptp-test/agent-test.sh FIXTURES
set -u
HERE=$(dirname "$(readlink -f "$0")")
VM=$HERE/../vm
FIXTURES=$1
hmp() { python3 "$VM/qmp.py" hmp "$@" >/dev/null; }
plug() { hmp "device_add usb-mtp,bus=xhci.0,rootdir=$FIXTURES/phone,readonly=on,desc=TestPhone,id=phone0"; sleep 4; }
unplug() { hmp "device_del phone0"; sleep 3; }
vm() { "$VM/ssh.sh" "$@"; }
running() { vm 'roster | grep -c "app/Photos"'; }
messages() { vm 'grep -c arrived /tmp/fake-photos.log 2>/dev/null'; }
failed=0
check() {	# what, value, expected
	if [[ $2 == "$3" ]]; then echo "ok   $1"; else echo "FAIL $1: $2, expected $3"; failed=1; fi
}

vm 'hey Photos quit >/dev/null 2>&1; kill $(ps | grep "[p]hotos_agent" | awk "{print \$(NF-3)}") 2>/dev/null
	rm -f /tmp/fake-photos.log ~/config/settings/Photos/agent; sleep 1'
plug
vm 'cd /Data/ptp && (./photos_agent > agent.log 2>&1 &); sleep 3'
check "a camera that is there when the agent starts opens nothing" "$(running)" 0
unplug
plug
check "an arrival starts Photos" "$(running)" 1
check "... with the camera's message" "$(messages)" 1
vm 'cat /tmp/fake-photos.log'
unplug
plug
check "while Photos runs the agent leaves it alone" "$(messages)" 1
vm 'hey Photos quit >/dev/null; mkdir -p ~/config/settings/Photos
	echo "open_on_connect false" > ~/config/settings/Photos/agent; sleep 1'
unplug
plug
check "open_on_connect false: nothing is started" "$(running)" 0
vm 'echo "open_on_connect true" > ~/config/settings/Photos/agent'
unplug
plug
check "open_on_connect true: Photos is started" "$(running)" 1
check "... with a message" "$(messages)" 2
# device churn: the agent has to be there, the same size, afterwards
areas() { vm 'team=$(ps | grep "[p]hotos_agent" | awk "{print \$(NF-3)}"); echo "team $team: $(listarea $team | wc -l) areas, $(ps | grep "[p]hotos_agent" | awk "{print \$(NF-2)}") threads"'; }
vm 'hey Photos quit >/dev/null'
before=$(areas)
for i in $(seq 15); do hmp "device_del phone0"; sleep 1; hmp "device_add usb-mtp,bus=xhci.0,rootdir=$FIXTURES/phone,readonly=on,desc=TestPhone,id=phone0"; sleep 1.5; vm 'hey Photos quit >/dev/null 2>&1'; done
after=$(areas)
check "the agent is the same after 15 more arrivals" "$after" "$before"
vm 'ps | grep "[p]hotos_agent"; hey Photos quit >/dev/null; rm -f ~/config/settings/Photos/agent'
unplug
exit $failed
