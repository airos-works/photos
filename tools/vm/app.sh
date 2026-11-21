#!/usr/bin/env bash
# (Re)starts Photos in the test VM on the test library (/Data/lib, settings
# in /Data/settings), never uploading to the Immich server it may be signed
# in to. app.sh stop only stops it.
HERE=$(dirname "$(readlink -f "$0")")
"$HERE/ssh.sh" 'for t in $(ps | grep "/Data/photos/Photos" | grep -v grep | awk "{print \$(NF-3)}"); do kill $t; done; sleep 1; true'
[[ ${1:-} == stop ]] && exit 0
"$HERE/ssh.sh" "cd /Data/photos && export PHOTOS_LIBRARY=/Data/lib PHOTOS_SETTINGS_DIR=/Data/settings PHOTOS_NO_UPLOAD=1 ${APP_ENV:-}; (/Data/photos/Photos $* > /Data/photos.log 2>&1 &); sleep ${APP_WAIT:-4}; ps | grep -c '[/]Data/photos/Photos'"
