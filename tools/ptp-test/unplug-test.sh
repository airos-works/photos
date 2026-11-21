#!/usr/bin/env bash
# Pulls QEMU's usb-mtp device out of the test VM in the middle of downloads,
# several times, and checks that ptpls comes back each time with an error
# and without a damaged file (docs/DEVICES.md).
#
#   VM_NAME=ptp tools/ptp-test/unplug-test.sh FIXTURES [rounds]
#
# FIXTURES is what make-fixtures.py wrote; ptpls is expected in /Data/ptp of
# the VM. A ptpls whose download came back but whose team does not end is
# counted apart: that is usb_raw holding a thread (see docs/DEVICES.md), and
# does not fail the test.
set -u
HERE=$(dirname "$(readlink -f "$0")")
VM=$HERE/../vm
FIXTURES=$1
ROUNDS=${2:-6}
hmp() { python3 "$VM/qmp.py" hmp "$@" >/dev/null; }
plug() { hmp "device_add usb-mtp,bus=xhci.0,rootdir=$FIXTURES/phone,readonly=on,desc=TestPhone,id=phone0"; }

failed=0
kept=0
for round in $(seq "$ROUNDS"); do
	plug; sleep 2
	delay=0.$((RANDOM % 9 + 1))
	# what hangs already
	before=$("$VM/ssh.sh" 'ps | grep -c "[p]tpls get"')
	# ptpls writes its errors and its last line ("... N failed") to stderr
	# as they happen: they are in the log even if its team does not end
	"$VM/ssh.sh" "cd /Data/ptp && rm -rf unplug unplug.log && (for i in 1 2 3 4 5 6 7 8 9 10; do ./ptpls get 0 all unplug >> unplug.log 2>&1; done > /dev/null 2>&1 &) ; sleep $delay"
	hmp "device_del phone0"
	sleep 30
	result=$("$VM/ssh.sh" 'cd /Data/ptp; ps | grep -c "[p]tpls get"; grep -c "failed$\|^ptpls: " unplug.log; grep -c "was disconnected\|is not connected\|no camera" unplug.log; cd unplug 2>/dev/null && sha1sum * 2>/dev/null')
	hung=$(($(echo "$result" | sed -n 1p) - before))
	returned=$(echo "$result" | sed -n 2p)
	reported=$(echo "$result" | sed -n 3p)
	# every file that is there is complete
	bad=$(echo "$result" | tail -n +4 | while read -r sum name; do
		grep -q "^$sum  .*/$name  " "$FIXTURES/sha1.txt" || echo "$name"
	done)
	files=$(echo "$result" | tail -n +4 | wc -l)
	echo "round $round: unplugged after $delay s: $returned lines from runs that returned, $reported about the disconnection, $files files left, damaged: ${bad:-none}, teams that did not end: $hung"
	kept=$((kept + hung))
	# the run that was cut off returned, and nothing that is left is
	# damaged
	[[ $returned -ge 1 && -z $bad ]] || failed=1
done
[[ $kept == 0 ]] || echo "$kept teams did not end after their work was done (usb_raw, docs/DEVICES.md)"
exit $failed
