#!/usr/bin/env bash
# raw-host.sh CARGO-ARGS...: cargo in raw/ for the build host (tests, checks).
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
. "$ROOT/tools/raw-env.sh" host
cd "$ROOT/raw"
exec cargo "$@"
