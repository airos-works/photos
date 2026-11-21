#!/usr/bin/env bash
# Fetches a few CC0 RAW samples (raw.pixls.us; the files and SHA-256 values
# LightCraft's `cargo xtask corpus` pins) for the tests of raw/ and for
# rawtest. They are never committed:
#
#   tools/raw-test/fetch-samples.sh [DIR]      default: raw/target/samples
#
# `cargo test` in raw/ reads PHOTOS_RAW_SAMPLES (default: that directory) and
# skips the file tests when it is not there.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
DIR=${1:-${PHOTOS_RAW_SAMPLES:-$ROOT/raw/target/samples}}
mkdir -p "$DIR"
# name  sha256  url
SAMPLES='
iphone6s.dng 845fee4f09c832af3728778737f5ae9b99124debb821b6e2066b94aed1daa91c https://raw.pixls.us/getfile.php/915/nice/Apple%20-%20iPhone%206s%20Plus%20-%2016bit%20%284:3%29.DNG
canon-40d.cr2 775c806358fedddec7622113a5e399a3330bd5a65e35e37ae1108d8bf58067be https://raw.pixls.us/data/Canon/EOS%2040D/_MG_0153.CR2
nikon-d70.nef dd6405aeb33b0cd5bf66c98ba98ccbb478a765450cfd130810e470dab8d1f4b4 https://raw.pixls.us/getfile.php/2060/nice/Nikon%20-%20D70%20-%2012bit%2012bit%20compressed%20%28Lossy%20%28type%201%29%29%20%283:2%29.NEF
fuji-xt20.raf a23045101b2912e64f154100d646c228c0d16585d21e95e0d1a006e537f7784c https://raw.pixls.us/getfile.php/1178/nice/Fujifilm%20-%20X-T20%20-%2014bit%2014bit%20compressed%20%283:2%29.RAF
fuji-xpro1.raf 0b1046c299d9eb222a8f003bd047f2a34f507da0d1704ac32cefbc82f799d121 https://raw.pixls.us/getfile.php/1191/nice/Fujifilm%20-%20X-Pro1%20-%2012bit%2012bit%20uncompressed%20%283:2%29.RAF
sony-rx100.arw 579a485b5126a25cbd55cbd5dadfa7d09cf021c99cc7d4869f9e56e3f759390b https://raw.pixls.us/data/Sony/DSC-RX100/DSC00838.ARW
panasonic-gx80.rw2 6e9a419c70c2124630912b2934027c635d3871e3d22dd91fed0655d58cfb5382 https://raw.pixls.us/getfile.php/1569/nice/Panasonic%20-%20DMC-GX80%20-%204:3.RW2
'
status=0
while read -r name sha url; do
	[[ -n $name ]] || continue
	out=$DIR/$name
	if [[ ! -f $out ]]; then
		curl -fsSL -o "$out.part" "$url" && mv "$out.part" "$out" || { echo "$name: download failed" >&2; status=1; continue; }
	fi
	if [[ $(sha256sum "$out" | cut -d' ' -f1) != "$sha" ]]; then
		echo "$name: SHA-256 differs from the pinned value (file removed)" >&2
		rm -f "$out"; status=1
	else
		printf '%s %s\n' "$name" "$(stat -c %s "$out")"
	fi
done <<< "$SAMPLES"
exit $status
