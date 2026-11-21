#!/usr/bin/env bash
# The camera layer against QEMU's devices in the test VM (docs/DEVICES.md):
# a phone (usb-mtp serving FIXTURES/phone) and a memory card (usb-storage
# with FIXTURES/card.img). Deploys build-x86_64/{ptpls,photos_agent,
# fake_photos} to /Data/ptp first.
#
#   VM_NAME=ptp tools/ptp-test/vm-test.sh FIXTURES
#
# FIXTURES is what make-fixtures.py wrote. The card is mounted by clicking
# the automounter's "Mount read-only" button at its place on the 1280x800
# screen.
set -u
HERE=$(dirname "$(readlink -f "$0")")
ROOT=$HERE/../..
VM=$HERE/../vm
FIXTURES=$1
hmp() { python3 "$VM/qmp.py" hmp "$@" >/dev/null; }
vm() { "$VM/ssh.sh" "$@"; }
failed=0
check() {	# what, value, expected
	if [[ $2 == "$3" ]]; then echo "ok   $1"; else echo "FAIL $1: $2, expected $3"; failed=1; fi
}
# sha1 lines "SUM  NAME" on stdin against the fixtures: "N of N"
compare() {
	local total=0 good=0 sum name
	while read -r sum name; do
		[[ ${#sum} == 40 ]] || continue
		total=$((total + 1))
		grep -q "^$sum  .*/$name  " "$FIXTURES/sha1.txt" && good=$((good + 1))
	done
	echo "$good of $total"
}
media=$(grep -c -i -E '\.(jpg|mov|nef)  ' "$FIXTURES/sha1.txt")
ascii=$(grep -i -E '\.(jpg|mov|nef)  ' "$FIXTURES/sha1.txt" | LC_ALL=C grep -c -v '[^ -~]')

vm 'mkdir -p /Data/ptp/app; hey Photos quit >/dev/null 2>&1
	kill $(ps | grep "[p]hotos_agent" | awk "{print \$(NF-3)}") 2>/dev/null; true'
"$VM/scp.sh" "$ROOT/build-x86_64/ptpls" "$ROOT/build-x86_64/photos_agent" vm:/Data/ptp/
"$VM/scp.sh" "$ROOT/build-x86_64/fake_photos" vm:/Data/ptp/app/Photos
vm 'cd /Data/ptp && mimeset -F app/Photos && mimeset -F photos_agent'

echo "== a phone (MTP)"
vm 'cd /Data/ptp; (./ptpls --watch > watch.log 2>&1 &); sleep 1'
hmp "device_add usb-mtp,bus=xhci.0,rootdir=$FIXTURES/phone,readonly=on,desc=TestPhone,id=phone0"
sleep 4
check "it is found" "$(vm 'cd /Data/ptp; ./ptpls | grep -c "ptp.*usb:46f4:0004"')" 1
check "its arrival is reported" "$(vm 'grep -c "^arrived usb:46f4:0004.* ptp$" /Data/ptp/watch.log')" 1
vm 'cd /Data/ptp; ./ptpls info 0 2>&1 | head -5'
# QEMU sends names through the C locale: the one that is not ASCII arrives
# as "Oops" and is no picture any more.
check "pictures and videos listed" "$(vm 'cd /Data/ptp; ./ptpls list 0 2>/dev/null | wc -l')" "$ascii"
check "thumbnails from the files' EXIF blocks" \
	"$(vm 'cd /Data/ptp; ./ptpls thumbs 0 2>/dev/null | grep -c " bytes"')" \
	"$(grep -c -E '/(IMG_|BIG_|Holiday)' "$FIXTURES/sha1.txt")"
vm 'cd /Data/ptp; rm -rf out; ./ptpls get 0 all out 2>&1 | tail -1'
check "downloads are the files" "$(vm 'cd /Data/ptp/out && sha1sum *' | compare)" "$ascii of $ascii"
vm 'cd /Data/ptp; ./ptpls cancel 0 BIG_0001.JPG 4000000 2>/dev/null'
check "a cancelled download leaves a usable device" "$?" 0
vm 'cd /Data/ptp; ./ptpls delete 0 IMG_0003.JPG 2>&1 | tail -1'
check "deleting from a read-only device fails" "$(vm 'cd /Data/ptp; ./ptpls delete 0 IMG_0003.JPG >/dev/null 2>&1; echo $?')" 1
hmp "device_del phone0"
sleep 3
check "its removal is reported" "$(vm 'grep -c "^removed usb:46f4:0004" /Data/ptp/watch.log')" 1

echo "== unplugged during downloads"
"$HERE/unplug-test.sh" "$FIXTURES" 4 || failed=1

echo "== a memory card (FAT32)"
hmp "drive_add 0 if=none,id=card$$,file=$FIXTURES/card.img,format=raw,readonly=on"
hmp "device_add usb-storage,bus=xhci.0,drive=card$$,id=card,removable=on"
sleep 8
check "not mounted before the automounter's question is answered" "$(vm 'df | grep -c -i testcard')" 0
python3 "$VM/qmp.py" click 789 272 >/dev/null
for i in $(seq 10); do sleep 2; [[ $(vm 'df | grep -c -i testcard') == 1 ]] && break; done
sleep 1
check "its arrival is reported once it is mounted" "$(vm 'grep -c "^arrived volume:testcard" /Data/ptp/watch.log')" 1
# This system's FAT driver gives the name that is not ASCII as something
# that cannot be opened: one file less.
check "pictures and videos listed" "$(vm 'cd /Data/ptp; ./ptpls list 0 2>/dev/null | wc -l')" "$ascii"
vm 'cd /Data/ptp; rm -rf card-out; ./ptpls get 0 all card-out 2>&1 | tail -1'
check "copies are the files" "$(vm 'cd /Data/ptp/card-out && sha1sum *' | compare)" "$ascii of $ascii"
check "thumbnails" "$(vm 'cd /Data/ptp; ./ptpls thumbs 0 2>/dev/null | grep -c " bytes"')" \
	"$(grep -c -E '/(IMG_|BIG_|Holiday)' "$FIXTURES/sha1.txt")"
hmp "device_del card"
sleep 5
check "its removal is reported" "$(vm 'grep -c "^removed volume:testcard" /Data/ptp/watch.log')" 1
vm 'kill $(ps | grep "[p]tpls --watch" | awk "{print \$(NF-3)}") 2>/dev/null; true'

echo "== the agent"
"$HERE/agent-test.sh" "$FIXTURES" || failed=1
echo "$media media files in the fixtures, $ascii with ASCII names"
[[ $failed == 0 ]] && echo "all passed" || echo "FAILED"
exit $failed
