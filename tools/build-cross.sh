#!/usr/bin/env bash
# Cross-compiles Photos on Linux with an air/OS cross SDK (airos-ci
# sdk/build-sdk.sh) and the third-party dependency prefix (libcurl, SQLite),
# the way the build server does. Produces build-ARCH/Photos, photos_agent and
# photos_tool.
#
#   tools/build-cross.sh [x86_64|arm64] [make target]
#
# AIROS_SDK defaults to /data1/airos/sdk, AIROS_DEPS to /data1/airos/deps.
# The RAW library (raw/, Rust) is built by tools/build-raw.sh when a Rust
# toolchain is there; SKIP_RAW=1 leaves it out (RAW files then show their
# embedded previews only).
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
ARCH=${1:-x86_64}
SDK=${AIROS_SDK:-/data1/airos/sdk}/$ARCH
DEPS=${AIROS_DEPS:-/data1/airos/deps}/$ARCH/boot/system
[[ -f $SDK/env.sh ]] || { echo "no air/OS SDK at $SDK" >&2; exit 1; }
# shellcheck disable=SC1091
. "$SDK/env.sh"
# The SDK's host tools live in HOST_TOOLS/bin on newer SDKs.
RCBIN=$TOOLS/rc/rc; XRESBIN=$TOOLS/xres; MIMESETBIN=$TOOLS/mimeset
[[ -x $RCBIN ]] || RCBIN=${HOST_TOOLS:-$TOOLS}/bin/rc
[[ -x $XRESBIN ]] || XRESBIN=${HOST_TOOLS:-$TOOLS}/bin/xres
[[ -x $MIMESETBIN ]] || MIMESETBIN=${HOST_TOOLS:-$TOOLS}/bin/mimeset

# A private farm with just libcurl and SQLite, so -lcurl is unambiguous.
FARM=$ROOT/build-$ARCH/.deps
mkdir -p "$FARM/lib" "$FARM/include"
ln -sf "$(ls "$DEPS"/lib/libcurl.so.4 "$DEPS"/develop/lib/libcurl.so.4 2>/dev/null | head -1)" "$FARM/lib/libcurl.so"
ln -sf "$(ls "$DEPS"/lib/libsqlite3.so.0 "$DEPS"/develop/lib/libsqlite3.so.0 2>/dev/null | head -1)" "$FARM/lib/libsqlite3.so"
ln -sfn "$DEPS/develop/headers/curl" "$FARM/include/curl"
ln -sf "$DEPS/develop/headers/sqlite3.h" "$FARM/include/sqlite3.h"

cd "$ROOT"
RAW_NATIVE=""
if [[ -z ${SKIP_RAW:-} && -x tools/build-raw.sh ]]; then
	tools/build-raw.sh "$ARCH"
fi
[[ -f build-$ARCH/libphotos_raw.libs ]] && RAW_NATIVE=$(cat "build-$ARCH/libphotos_raw.libs")

exec make -s BUILD="build-$ARCH" -j"$(nproc)" \
	CXX="${CROSS}g++ --sysroot=$SYSROOT -specs=$UNWIND_SPECS" \
	APP_CPPFLAGS="-I$FARM/include" \
	APP_LDFLAGS="-L$FARM/lib -Wl,-rpath-link,$FARM/lib -Wl,-rpath-link,$DEPS/lib" \
	RAW_NATIVE_LIBS="$RAW_NATIVE" \
	RC="$RCBIN" XRES="$XRESBIN" MIMESET="$MIMESETBIN --mimedb $MIMEDB" \
	"${2:-all}"
