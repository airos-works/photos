# Sourced by tools/build-raw.sh and by hand for host work on raw/:
#   . tools/raw-env.sh [x86_64|arm64|host]
# The Rust toolchain is the one airos-ci pins (sdk/build-rust.sh:
# $AIROS_TOOLCHAIN/rust, a nightly with rust-src); the registry and git
# caches go to a cargo home of our own when the toolchain's belongs to the
# CI runner. Nothing is written outside raw/target and that cargo home.
RAW_ARCH=${1:-${RAW_ARCH:-x86_64}}
AIROS_SDK=${AIROS_SDK:-/data1/airos/sdk}
AIROS_TOOLCHAIN=${AIROS_TOOLCHAIN:-/data1/airos/toolchain}
_raw_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
_raw_rust_arch=$RAW_ARCH
[[ $RAW_ARCH == host ]] && _raw_rust_arch=x86_64
if [[ -f $AIROS_TOOLCHAIN/rust/env-$_raw_rust_arch.sh ]]; then
	# shellcheck disable=SC1090
	. "$AIROS_TOOLCHAIN/rust/env-$_raw_rust_arch.sh"
else
	echo "raw-env: no Rust toolchain at $AIROS_TOOLCHAIN/rust (airos-ci sdk/build-rust.sh $_raw_rust_arch)" >&2
	return 1 2>/dev/null || exit 1
fi
export RUST_TARGET
# The toolchain's cargo home may belong to the CI runner: keep its
# toolchain, use our own registry.
if [[ ! -w $CARGO_HOME/registry ]]; then
	export PATH="$CARGO_HOME/bin:$PATH"
	for _raw_home in "${PHOTOS_CARGO_HOME:-}" /mnt/HaikuWork/toolchains/rust/cargo "$_raw_root/raw/target/cargo-home"; do
		[[ -n $_raw_home ]] || continue
		mkdir -p "$_raw_home" 2>/dev/null || continue
		[[ -w $_raw_home ]] && { export CARGO_HOME=$_raw_home; break; }
	done
fi
export CARGO_TARGET_DIR=${PHOTOS_RAW_TARGET_DIR:-$_raw_root/raw/target}
if [[ $RAW_ARCH != host ]]; then
	# shellcheck disable=SC1090
	. "$AIROS_SDK/$RAW_ARCH/env.sh"
	export AIROS_SYSROOT=$SYSROOT AIROS_CROSS=$CROSS AIROS_UNWIND_SPECS=$UNWIND_SPECS
	_raw_triple=${RUST_TARGET//-/_}
	export "CARGO_TARGET_${_raw_triple^^}_LINKER=$_raw_root/tools/raw-cc"
	export "CC_$_raw_triple=$_raw_root/tools/raw-cc" "CXX_$_raw_triple=$_raw_root/tools/raw-c++"
	export "AR_$_raw_triple=${CROSS}ar" "RANLIB_$_raw_triple=${CROSS}ranlib"
	# arm64 is a custom target: its specification is in raw/targets when the
	# toolchain's own copy (the CI checkout) is not there.
	if [[ $RAW_ARCH == arm64 && ! -f ${RUST_TARGET_PATH:-/nonexistent}/$RUST_TARGET.json ]]; then
		export RUST_TARGET_PATH=$_raw_root/raw/targets
	fi
	unset _raw_triple
fi
unset _raw_root _raw_rust_arch _raw_home
