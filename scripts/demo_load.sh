#!/usr/bin/env bash
# Heavy Blender offload for a live mesh demo.
# Run on the requester after donors are ADMITTED.
#
#   ./scripts/demo_load.sh
#   ./scripts/demo_load.sh --frames 1-480 --chunk 8
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/scripts/demo_env.sh"

FRAMES="${FRAMES:-1-240}"
CHUNK="${CHUNK:-8}"
CORES="${CORES:-4}"
OUT="${OUT:-/tmp/zeromesh-demo-load}"
SCENE="${SCENE:-$ROOT/apps/zm-render/tests/data/spin_cube.blend}"
ENGINE="${ENGINE:-CYCLES}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --frames) FRAMES="$2"; shift 2 ;;
    --chunk) CHUNK="$2"; shift 2 ;;
    --cores) CORES="$2"; shift 2 ;;
    -o|--output) OUT="$2"; shift 2 ;;
    --scene) SCENE="$2"; shift 2 ;;
    --engine) ENGINE="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: demo_load.sh [--frames A-B] [--chunk N] [--cores N] [--engine CYCLES] [-o DIR]"
      exit 0
      ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

pick_bin() {
  local name="$1"
  if [[ -x "$ROOT/build-verify/$name" ]]; then echo "$ROOT/build-verify/$name"; return; fi
  if [[ -x "$ROOT/build/$name" ]]; then echo "$ROOT/build/$name"; return; fi
  command -v "$name" || true
}

ZM="$(pick_bin zm)"
ZMD="$(pick_bin zmd)"
ZM_RENDER="$(command -v zm-render || true)"
die() { echo "load: $*" >&2; exit 1; }

[[ -n "$ZM" && -n "$ZMD" ]] || die "need zm/zmd"
[[ -n "$ZM_RENDER" ]] || die "need zm-render"
[[ -f "$SCENE" ]] || die "missing scene $SCENE"

if ! "$ZM" status >/dev/null 2>&1; then
  echo "load: starting zmd…"
  "$ZMD" >/tmp/zeromesh-demo-load-zmd.log 2>&1 &
  sleep 1
fi

echo "load: peers (want several ADMITTED)"
"$ZM" status || true

# Rough task count for the pitch narrative.
IFS='-:' read -r _a _b <<<"${FRAMES/-/ }"
# shellcheck disable=SC2086
start=${FRAMES%%-*}; end=${FRAMES##*-}
tasks=$(( (end - start + CHUNK) / CHUNK ))
echo "load: $FRAMES in chunks of $CHUNK → ~$tasks remote tasks, engine=$ENGINE cores/task=$CORES"

mkdir -p "$OUT"
echo "load: submitting (remote-only)…"
"$ZM_RENDER" "$SCENE" \
  --frames "$FRAMES" \
  --chunk "$CHUNK" \
  --cores "$CORES" \
  --engine "$ENGINE" \
  -o "$OUT" \
  --remote-only \
  >/tmp/zeromesh-demo-load-render.log 2>&1 &
echo "load: render pid $!  log /tmp/zeromesh-demo-load-render.log"
echo "load: monitor — watch chunks land on donors"
exec "$ZM" monitor
