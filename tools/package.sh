#!/usr/bin/env bash
# Makes the air/OS package from a cross build, by hand:
#
#   tools/build-cross.sh x86_64 && tools/package.sh x86_64
#     -> artifacts/photos-<version>-<arch>.hpkg
#
# The build server makes the same package with airos-ci
# (apps/build-app-packages.sh, build_photos): keep the two in step. This
# script takes no lock of the build server and writes only under build-ARCH
# and artifacts (and, as every use of the SDK's host tools does, the SDK's
# attribute store for the files it stages).
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
ARCH=${1:-x86_64}
SDK=${AIROS_SDK:-/data1/airos/sdk}/$ARCH
[[ -f $SDK/env.sh ]] || { echo "no air/OS SDK at $SDK" >&2; exit 1; }
# shellcheck disable=SC1091
. "$SDK/env.sh"
B=$ROOT/build-$ARCH
STAGE=$B/stage
OUT=$ROOT/artifacts
STRIP=${CROSS}strip
for file in Photos photos_agent photos_tool Photos.rsrc photos_agent.rsrc; do
	[[ -e $B/$file ]] || { echo "no $B/$file: run tools/build-cross.sh $ARCH" >&2; exit 1; }
done

[[ -e $STAGE ]] && "$TOOLS/rm_attrs" -rf "$STAGE"
mkdir -p "$STAGE" "$OUT"
touch "$B/.stage-began"
sleep 0.05

install_binary() {	# <file> <resources or -> <path in package>
	local dest=$STAGE/$3
	mkdir -p "$(dirname "$dest")"
	cp "$1" "$dest"
	chmod 755 "$dest"
	"$STRIP" --strip-debug "$dest"
	# GNU strip drops the appended Haiku resources; put them back.
	[[ $2 == - ]] || "$TOOLS/xres" -o "$dest" "$2"
}
install_file() {	# <file> <path in package>
	mkdir -p "$(dirname "$STAGE/$2")"
	cp -R "$1" "$STAGE/$2"
	chmod 644 "$STAGE/$2"
}

install_binary "$B/Photos" "$B/Photos.rsrc" apps/Photos
install_binary "$B/photos_agent" "$B/photos_agent.rsrc" bin/photos_agent
install_binary "$B/photos_tool" - bin/photos_tool
for model in "$ROOT"/models/*.tflite; do
	install_file "$model" "data/Photos/models/$(basename "$model")"
done
install_file "$ROOT/resources/photos_agent.launch" data/user_launch/photos_agent
for doc in README.md LICENSE docs/DESIGN.md docs/IMMICH.md docs/FACES.md docs/RAW.md \
		docs/DEVICES.md docs/TESTING.md models/README.md models/LICENSE.YuNet \
		models/LICENSE.SFace raw/NOTICE; do
	[[ -e $ROOT/$doc ]] && install_file "$ROOT/$doc" "documentation/packages/photos/$doc"
done
mkdir -p "$STAGE/data/deskbar/menu/Applications"
ln -s ../../../../apps/Photos "$STAGE/data/deskbar/menu/Applications/Photos"

# x86_64 keeps its requires (HaikuPorts provides them); arm64 has no
# repository for lib: providers and an unresolvable package is deactivated at
# boot, so it asks for the system only (the images ship the libraries).
if [[ $ARCH == arm64 ]]; then
	awk '
		/^architecture[ \t]/ { print "architecture arm64"; next }
		/^requires[ \t]*\{/  { print; print "\thaiku >= r1~beta6"; skip = 1; next }
		skip && /^[ \t]*\}/  { print; skip = 0; next }
		skip { next }
		{ print }' "$ROOT/resources/Photos.PackageInfo" > "$STAGE/.PackageInfo"
else
	sed "s/^architecture[ \t].*/architecture $ARCH/" "$ROOT/resources/Photos.PackageInfo" \
		> "$STAGE/.PackageInfo"
fi

# The host tools keep attributes in $ATTRS by inode; an entry left by a file
# that had the inode before must not end up on a staged one.
purge_stale_attributes() {
	local entry dir
	while IFS= read -r -d '' entry; do
		dir=$ATTRS/$(stat -c %i "$entry")
		[[ -L $entry ]] && dir=$ATTRS/symlink-$(stat -c %i "$entry")
		[[ -d $dir ]] || continue
		find "$dir" -mindepth 1 ! -newer "$1" -exec rm -rf {} + 2>/dev/null || true
	done < <(find "$STAGE" -print0)
}
purge_stale_attributes "$B/.stage-began"
# Signature, flags, icon, version and types as attributes, and the two
# applications in the package's MIME database, as mimeset does on Haiku.
"$TOOLS/resattr/resattr" -O -o "$STAGE/apps/Photos" "$B/Photos.rsrc"
"$TOOLS/resattr/resattr" -O -o "$STAGE/bin/photos_agent" "$B/photos_agent.rsrc"
( cd "$STAGE" && "$TOOLS/mimeset" --all -f --mimedb data/mime_db --mimedb "$MIMEDB" \
	apps/Photos bin/photos_agent )
purge_stale_attributes "$B/.stage-began"

version=$(awk '$1 == "version" { print $2; exit }' "$STAGE/.PackageInfo")
package=$OUT/photos-$version-$ARCH.hpkg
rm -f "$OUT"/photos-[0-9]*-"$ARCH".hpkg
"$TOOLS/package/package" create -q -C "$STAGE" "$package"
"$TOOLS/package/package" list -i "$package" | grep -q "architecture: $ARCH" \
	|| { echo "$package is not $ARCH" >&2; exit 1; }
# Only the applications (BEOS:*) and the MIME database (META:*) may carry
# attributes.
"$TOOLS/package/package" list -a "$package" | awk -v file="$package" '
	/^[ \t]*</ {
		attr = $0; sub(/^[ \t]*</, "", attr); sub(/[ \t].*/, "", attr)
		ok = (path ~ /^(apps|bin)\// && attr ~ /^BEOS:/) \
			|| (path ~ /^data\/mime_db\// && attr ~ /^META:/)
		if (!ok) { print "unexpected attribute " attr " on " path " in " file > "/dev/stderr"; bad = 1 }
		next
	}
	/^[ \t]*[^ \t]/ && started {
		match($0, /^ */); depth = RLENGTH / 2
		entry = substr($0, RLENGTH + 1); sub(/[ \t][ \t]+.*/, "", entry)
		stack[depth] = entry; path = stack[0]
		for (i = 1; i <= depth; i++) path = path "/" stack[i]
		next
	}
	/^[^ \t]/ { started = 1; match($0, /^ */); stack[0] = $1; path = $1 }
	END { exit bad }
' || { echo "unexpected attributes in $package" >&2; exit 1; }
echo "$package ($(du -h "$package" | cut -f1))"
