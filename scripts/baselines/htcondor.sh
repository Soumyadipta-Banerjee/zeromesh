#!/usr/bin/env bash
# Rootless HTCondor baseline in a Podman container (docker.io/htcondor/mini).
#   htcondor.sh up [tuned]   start the pool (tuned = apply condor_tuned.conf)
#   htcondor.sh submit FILE  run a submit file as the pool's submit user and wait for it
#   htcondor.sh status       show the pool and queue
#   htcondor.sh down         remove the container
set -euo pipefail

IMAGE=docker.io/htcondor/mini:lts
NAME=${ZM_CONDOR_NAME:-zm-condor}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
as_submitter() { podman exec -u submituser "$NAME" "$@"; }

case "${1:-}" in
up)
    podman rm -f "$NAME" >/dev/null 2>&1 || true
    args=()
    [[ ${2:-} == tuned ]] && args+=(-v "$HERE/condor_tuned.conf:/etc/condor/config.d/99-zm-tuned.conf:ro,Z")
    podman run -d --name "$NAME" "${args[@]}" "$IMAGE" >/dev/null
    for _ in $(seq 30); do
        podman exec "$NAME" condor_status -total >/dev/null 2>&1 && break
        sleep 1
    done
    podman exec "$NAME" condor_version | head -1
    ;;
submit)
    [[ -f ${2:-} ]] || { echo "usage: $0 submit FILE" >&2; exit 2; }
    podman cp "$2" "$NAME:/home/submituser/job.sub"
    podman exec "$NAME" chown submituser /home/submituser/job.sub
    as_submitter sh -c 'cd ~ && condor_submit job.sub && condor_wait -wait 600 "$(sed -n "s/^log *= *//p" job.sub)"'
    ;;
status)
    podman exec "$NAME" condor_status -total
    as_submitter condor_q
    ;;
down)
    podman rm -f "$NAME"
    ;;
*)
    sed -n '2,6p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
    exit 2
    ;;
esac
