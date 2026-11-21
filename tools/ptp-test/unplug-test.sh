#!/usr/bin/env bash
# Pulls QEMU's usb-mtp device out of the test VM in the middle of downloads,
# several times, and checks that ptpls comes back each time with an error
# and no partial file (docs/DEVICES.md).
#
#   VM_NAME=ptp tools/ptp-test/unplug-test.sh FIXTURES [rounds]
#
# FIXTURES is what make-fixtures.py wrote; ptpls is expected in /Data/ptp of
# the VM.
set -u
HERE=$(dirname "$(readlink -f "$0")")
VM=$HERE/../vm
FIXTURES=$1
ROUNDS=${2:-6}
hmp() { python3 "$VM/qmp.py" hmp "$@" >/dev/null; }
plug() { hmp "device_add usb-mtp,bus=xhci.0,rootdir=$FIXTURES/phone,readonly=on,desc=TestPhone,id=phone0"; }

failed=0
# what hangs already (an earlier test on a system without the fix)
before=$("$VM/ssh.sh" 'ps | grep -c "[p]tpls get"')
for round in $(seq "$ROUNDS"); do
	plug; sleep 2
	delay=0.$((RANDOM % 9 + 1))
	"$VM/ssh.sh" "cd /Data/ptp && rm -rf unplug && (for i in 1 2 3 4 5 6 7 8 9 10; do ./ptpls get 0 all unplug 2>&1 | grep -v '^unplug/' | tail -3; done > unplug.log 2>&1 &) ; sleep $delay"
	hmp "device_del phone0"
	sleep 8
	result=$("$VM/ssh.sh" 'cd /Data/ptp; ps | grep -c "[p]tpls get"; grep -c "was disconnected\|is not connected\|no camera" unplug.log; cd unplug 2>/dev/null && sha1sum * 2>/dev/null')
	hung=$(($(echo "$result" | sed -n 1p) - before))
	reported=$(echo "$result" | sed -n 2p)
	# every file that is there is complete
	bad=$(echo "$result" | tail -n +3 | while read -r sum name; do
		grep -q "^$sum  .*/$name  " "$FIXTURES/sha1.txt" || echo "$name"
	done)
	files=$(echo "$result" | tail -n +3 | wc -l)
	echo "round $round: unplugged after $delay s: $hung hung, $reported errors reported, $files files left, damaged: ${bad:-none}"
	[[ $hung == 0 && -z $bad && $reported -gt 0 ]] || failed=1
done
exit $failed
