#!/usr/bin/env bash
# Builds the RAW library (raw/: LightCraft's decoder behind photos_raw.h) for
# air/OS and puts it where the Makefile looks for it:
#
#   tools/build-raw.sh [x86_64|arm64]
#     build-ARCH/libphotos_raw.a      the static library
#     build-ARCH/libphotos_raw.libs   what the final link needs after it
#
# The toolchain is airos-ci's (tools/raw-env.sh: the SDK's cross gcc as the
# linker, the pinned Rust nightly, std built from source for the Haiku
# target). The first build compiles LightCraft and what it depends on
# (minutes); after that a run with nothing changed takes about a second.
# Cargo's output stays in raw/target (PHOTOS_RAW_TARGET_DIR moves it).
#
#   PHOTOS_RAW_FEATURES   cargo features (default: the crate's, which include
#                         "heif"; "" builds without HEIF)
#   PHOTOS_RAW_OFFLINE=1  no network (everything is in the cargo home already)
#   JOBS                  parallel rustc jobs (default: cargo's)
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
ARCH=${1:-x86_64}
case $ARCH in x86_64|arm64) ;; *) echo "build-raw: unknown architecture $ARCH" >&2; exit 1 ;; esac
# shellcheck disable=SC1091
. "$ROOT/tools/raw-env.sh" "$ARCH"

OUT=$ROOT/build-$ARCH
LIB=$CARGO_TARGET_DIR/$RUST_TARGET/release/libphotos_raw.a
LIBS=$CARGO_TARGET_DIR/$RUST_TARGET/release/libphotos_raw.libs
LOG=$CARGO_TARGET_DIR/build-$ARCH.log
mkdir -p "$OUT" "$CARGO_TARGET_DIR"

args=(rustc --release --locked --lib --crate-type staticlib --target "$RUST_TARGET"
	-Zbuild-std=std,panic_abort)
[[ -z ${JOBS:-} ]] || args+=(-j "$JOBS")
[[ -z ${PHOTOS_RAW_OFFLINE:-} ]] || args+=(--offline)
if [[ -n ${PHOTOS_RAW_FEATURES+set} ]]; then
	args+=(--no-default-features)
	[[ -z $PHOTOS_RAW_FEATURES ]] || args+=(--features "$PHOTOS_RAW_FEATURES")
fi

build() {
	( cd "$ROOT/raw" && cargo "${args[@]}" -- --print native-static-libs ) > "$LOG" 2>&1 || {
		grep -v '^ *\(Compiling\|Downloaded\|Downloading\|Updating\|Locking\|Adding\) ' "$LOG" | tail -40 >&2
		echo "build-raw: cargo failed (log: $LOG)" >&2
		exit 1
	}
	# rustc names the system libraries a staticlib needs when it links one,
	# which it does not do when nothing changed: the last answer is kept.
	local native
	native=$(sed -n 's/^note: native-static-libs: //p' "$LOG" | tail -1)
	[[ -z $native ]] || printf '%s\n' "$native" > "$LIBS"
}
build
if [[ ! -s $LIBS ]]; then
	# (a library from before this script kept the answer: link it once more)
	touch "$ROOT/raw/src/lib.rs"
	build
fi
[[ -f $LIB && -s $LIBS ]] || { echo "build-raw: no library after the build (log: $LOG)" >&2; exit 1; }

# Only what Photos calls is linked in: the library is LightCraft's whole
# engine, of which the final link keeps the decoder and the CPU pipeline.
# A Haiku executable exports every global symbol, which would make all of
# the archive a root of that collection: --exclude-libs keeps the archive's
# symbols to the program itself. (noexecstack: as rustc links its own
# programs; without it ld warns about the SDK's crtn.o.)
native="-Wl,--gc-sections -Wl,--exclude-libs,libphotos_raw.a -Wl,-z,noexecstack $(cat "$LIBS")"
if [[ ! -f $OUT/libphotos_raw.a || $LIB -nt $OUT/libphotos_raw.a ]]; then
	cp -p "$LIB" "$OUT/libphotos_raw.a.new"
	mv "$OUT/libphotos_raw.a.new" "$OUT/libphotos_raw.a"
	echo "build-raw: $OUT/libphotos_raw.a ($(du -h "$LIB" | cut -f1))"
fi
[[ -f $OUT/libphotos_raw.libs && $(cat "$OUT/libphotos_raw.libs") == "$native" ]] \
	|| printf '%s\n' "$native" > "$OUT/libphotos_raw.libs"
