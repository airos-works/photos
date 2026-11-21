#!/usr/bin/env bash
# Builds facescan_host: the face pipeline with the Neural Kit's CPU
# interpreter on the build host (Linux), for measuring on many pictures.
#
#   tools/face-test/build-host.sh [OUT]      (default build-host/face-test)
#
# HAIKU_TREE (default /mnt/HaikuWork/src/haiku) is a checkout of
# airos-works/haiku with the Neural Kit; its own host test Makefile builds
# the kit's loader and interpreter.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
TREE=${HAIKU_TREE:-/mnt/HaikuWork/src/haiku}
HOST=$TREE/src/tests/kits/neural/host
OUT=$(mkdir -p "${1:-$ROOT/build-host/face-test}" && cd "${1:-$ROOT/build-host/face-test}" && pwd)
[[ -f $HOST/Makefile ]] || { echo "no Neural Kit host tests in $TREE" >&2; exit 1; }
make -s -C "$HOST" OUT="$OUT/kit" "$OUT/kit/neural_host" >/dev/null
KIT_OBJECTS=$(ls "$OUT"/kit/*.o | grep -v -E '/(neural_host|speech_host|Speech[A-Za-z]*)\.o$')
# shellcheck disable=SC2086
g++ -O2 -g -std=gnu++17 -Wall -Wextra -Wno-unused-parameter -Wno-multichar \
	-I"$ROOT/src" -I"$HOST/shim" -I"$TREE/headers/os" -I"$TREE/headers/private/neural" -pthread \
	"$ROOT/tools/face-test/facescan_host.cpp" "$ROOT/src/core/FaceMath.cpp" $KIT_OBJECTS \
	-o "$OUT/facescan_host" -lm
echo "$OUT/facescan_host"
