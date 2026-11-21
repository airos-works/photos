#!/usr/bin/env bash
# Runs the sync integration tests on the build host: starts
# tools/fake-immich.py, makes fixtures, builds and runs
# build-host/photos_integration.
#   tools/test-sync.sh [test name filter]
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
PORT=${PHOTOS_TEST_PORT:-18283}
WORK=${PHOTOS_TEST_WORK:-$ROOT/build-host/sync-test}
mkdir -p "$WORK"
[[ -d $WORK/fixtures/DCIM ]] || python3 "$ROOT/tools/make-fixtures.py" "$WORK/fixtures" 12 >/dev/null
python3 "$ROOT/tools/fake-immich.py" --port "$PORT" --bind 127.0.0.1 > "$WORK/server.log" 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null || true' EXIT
for i in $(seq 50); do
	curl -s -o /dev/null "http://127.0.0.1:$PORT/api/server/ping" && break
	sleep 0.1
done
cd "$ROOT"
make -s integration-host
PHOTOS_TEST_SERVER="http://127.0.0.1:$PORT" PHOTOS_TEST_FIXTURES="$WORK/fixtures" \
	PHOTOS_TEST_TMP="$WORK/tmp" build-host/photos_integration ${1:-}
