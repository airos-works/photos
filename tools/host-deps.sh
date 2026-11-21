#!/usr/bin/env bash
# Host tests need SQLite. A Linux machine usually has the library without the
# header: take the header from the air/OS dependency prefix (or the system)
# and link the host's library by path.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
D=$ROOT/build-host/.deps
mkdir -p "$D/include" "$D/lib"
if [[ ! -e $D/include/sqlite3.h ]]; then
	for h in /usr/include/sqlite3.h \
		"${AIROS_DEPS:-/data1/airos/deps}"/x86_64/boot/system/develop/headers/sqlite3.h; do
		[[ -f $h ]] && { cp "$h" "$D/include/sqlite3.h"; break; }
	done
fi
if [[ ! -e $D/lib/libsqlite3.so ]]; then
	for l in /usr/lib/x86_64-linux-gnu/libsqlite3.so /usr/lib/x86_64-linux-gnu/libsqlite3.so.0 \
		/usr/lib64/libsqlite3.so.0 /usr/lib/libsqlite3.so.0; do
		[[ -e $l ]] && { ln -sf "$l" "$D/lib/libsqlite3.so"; break; }
	done
fi
[[ -e $D/include/sqlite3.h && -e $D/lib/libsqlite3.so ]] \
	|| { echo "host-deps: SQLite header or library not found" >&2; exit 1; }
