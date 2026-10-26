#!/usr/bin/env bash
# Requester laptop: submit Blender work to donors, then monitor.
# Donors should run: ./scripts/setup_donor.sh [--join 'ZM-…']
#
#   ./scripts/setup_offload.sh
#   ./scripts/setup_offload.sh --frames 1-240 --chunk 16
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/scripts/demo_env.sh"

FRAMES="${FRAMES:-1-48}"
CHUNK="${CHUNK:-8}"
OUT="${OUT:-/tmp/zeromesh-offload-renders}"
SCENE="${SCENE:-$ROOT/apps/zm-render/tests/data/spin_cube.blend}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --frames) FRAMES="$2"; shift 2 ;;
    --chunk) CHUNK="$2"; shift 2 ;;
    -o|--output) OUT="$2"; shift 2 ;;
    --scene) SCENE="$2"; shift 2 ;;
    -h|--help)
      cat <<'EOF'
Usage: setup_offload.sh [--frames A-B] [--chunk N] [-o DIR] [--scene FILE]
Submits remote-only zm-render from this laptop, then opens zm monitor.
Donors: ./scripts/setup_donor.sh
EOF
      exit 0
      ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

pick_bin() {
  local name="$1"
  if [[ -x "$ROOT/build-verify/$name" ]]; then echo "$ROOT/build-verify/$name"; return; fi
  if [[ -x "$ROOT/build/$name" ]]; then echo "$ROOT/build/$name"; return; fi
  if command -v "$name" >/dev/null 2>&1; then command -v "$name"; return; fi
  echo ""
}

ZM="$(pick_bin zm)"
ZMD="$(pick_bin zmd)"
ZM_RENDER="$(command -v zm-render || true)"
die() { echo "offload: $*" >&2; exit 1; }

[[ -n "$ZM" && -n "$ZMD" ]] || die "need zm/zmd"
[[ -n "$ZM_RENDER" ]] || die "zm-render missing (pip install -e ./apps/zm-render)"
[[ -f "$SCENE" ]] || die "scene not found: $SCENE"

export ZM_BUILD_DIR="${ZM_BUILD_DIR:-$(dirname "$ZM")}"

if ! "$ZM" status >/dev/null 2>&1; then
  echo "offload: starting zmd…"
  "$ZMD" >/tmp/zeromesh-offload-zmd.log 2>&1 &
  for _ in $(seq 1 40); do
    "$ZM" status >/dev/null 2>&1 && break
    sleep 0.25
  done
  "$ZM" status >/dev/null 2>&1 || die "zmd failed — see /tmp/zeromesh-offload-zmd.log"
fi

echo "offload: peers (donors should show ADMITTED)"
"$ZM" status || true

mkdir -p "$OUT"
echo "offload: zm-render --remote-only $SCENE --frames $FRAMES --chunk $CHUNK -o $OUT"
"$ZM_RENDER" "$SCENE" --frames "$FRAMES" --chunk "$CHUNK" -o "$OUT" --remote-only \
  >/tmp/zeromesh-offload-render.log 2>&1 &
RENDER_PID=$!
echo "offload: render pid $RENDER_PID  log /tmp/zeromesh-offload-render.log"
echo "offload: monitor (q / Ctrl-C)"

trap 'kill -0 "$RENDER_PID" 2>/dev/null && echo "offload: render still running (pid $RENDER_PID)"' EXIT
exec "$ZM" monitor
