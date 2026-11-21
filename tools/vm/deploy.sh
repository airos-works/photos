#!/usr/bin/env bash
# Copies a cross build (build-x86_64) and the face models into the test VM:
# /Data/photos/{Photos,photos_tool,photos_agent,models}. Then e.g.
#   tools/vm/ssh.sh 'PHOTOS_LIBRARY=/Data/lib PHOTOS_SETTINGS_DIR=/Data/settings /Data/photos/photos_tool info'
set -euo pipefail
HERE=$(dirname "$(readlink -f "$0")")
ROOT=$(cd "$HERE/../.." && pwd)
B=$ROOT/build-${ARCH:-x86_64}
"$HERE/ssh.sh" 'mkdir -p /Data/photos'
# New names for running programs: overwriting a running binary confuses the
# runtime loader.
"$HERE/ssh.sh" 'cd /Data/photos && for f in Photos photos_tool photos_agent; do [ -e $f ] && mv -f $f $f.old; done; true'
"$HERE/scp.sh" "$B/Photos" "$B/photos_tool" "$B/photos_agent" vm:/Data/photos/
if ls "$ROOT"/models/*.tflite >/dev/null 2>&1; then
	"$HERE/ssh.sh" 'mkdir -p /Data/photos/models'
	"$HERE/scp.sh" "$ROOT"/models/*.tflite vm:/Data/photos/models/
fi
echo deployed
