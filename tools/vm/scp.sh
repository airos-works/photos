#!/usr/bin/env bash
# scp.sh LOCAL... vm:REMOTE  or  scp.sh vm:REMOTE LOCAL
. "$(dirname "$(readlink -f "$0")")/env.sh"
args=()
for a in "$@"; do args+=("${a/#vm:/user@127.0.0.1:}"); done
exec scp -q -r -i "$VM_BASE/id_ed25519" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
	-P "$VM_SSH_PORT" "${args[@]}"
