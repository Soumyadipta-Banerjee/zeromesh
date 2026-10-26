#!/usr/bin/env bash
# Back-compat wrapper — offload setup lives in setup_offload.sh.
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/setup_offload.sh" "$@"
