#!/usr/bin/env bash
# ssh.sh [command]: a shell in the test VM.
. "$(dirname "$(readlink -f "$0")")/env.sh"
exec ssh -q -i "$VM_BASE/id_ed25519" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
	-o ConnectTimeout=5 -p "$VM_SSH_PORT" user@127.0.0.1 "$@"
