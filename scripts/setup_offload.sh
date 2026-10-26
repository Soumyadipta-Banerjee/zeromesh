#!/usr/bin/env bash
# Prepare this laptop to offload Blender frames to the mesh, then watch.
#
#   ./scripts/setup_offload.sh
#   ./scripts/setup_offload.sh --frames 1-240 --chunk 16
#   ./scripts/setup_offload.sh --setup-only          # zmd + peers only (no render)
#
# Work goes to donors by default (--remote-only). Donor machines need:
#   source scripts/demo_env.sh && zmd
# and the same team (`zm team join …`).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/scripts/demo_env.sh"

FRAMES="${FRAMES:-1-48}"
CHUNK="${CHUNK:-8}"
OUT="${OUT:-/tmp/zeromesh-offload-renders}"
SCENE="${SCENE:-$ROOT/apps/zm-render/tests/data/spin_cube.blend}"
SETUP_ONLY=0
# Offload from this laptop: do not schedule chunks locally.
REMOTE_ONLY=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --frames) FRAMES="$2"; shift 2 ;;
    --chunk) CHUNK="$2"; shift 2 ;;
    -o|--output) OUT="$2"; shift 2 ;;
    --scene) SCENE="$2"; shift 2 ;;
    --setup-only) SETUP_ONLY=1; shift ;;
    --also-local) REMOTE_ONLY=0; shift ;;
    -h|--help)
      cat <<'EOF'
Usage: setup_offload.sh [options]
  --frames A-B     frame range (default 1-48)
  --chunk N        frames per task (default 8)
  -o DIR           output dir (default /tmp/zeromesh-offload-renders)
  --scene FILE     .blend path (default spin_cube.blend)
  --setup-only     start zmd / show peers; do not render or monitor
  --also-local     allow chunks on this laptop too (default: remote-only)
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

[[ -n "$ZM" && -n "$ZMD" ]] || die "need zm/zmd (ninja -C build-verify)"
[[ -n "$ZM_RENDER" ]] || die "zm-render missing (pip install -e ./apps/zm-render)"
command -v blender >/dev/null 2>&1 || die "blender not on PATH (donors need it for --env host)"
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

echo "offload: mesh status"
"$ZM" status || true

if [[ "$SETUP_ONLY" == "1" ]]; then
  echo "offload: setup done. Join donors with: zm team join '<code>'"
  echo "offload: then render with: zm-render $SCENE --frames $FRAMES --chunk $CHUNK -o $OUT --remote-only"
  exit 0
fi

mkdir -p "$OUT"
RENDER_ARGS=(--frames "$FRAMES" --chunk "$CHUNK" -o "$OUT")
[[ "$REMOTE_ONLY" == "1" ]] && RENDER_ARGS+=(--remote-only)

echo "offload: submitting $SCENE → donors (${RENDER_ARGS[*]})"
"$ZM_RENDER" "$SCENE" "${RENDER_ARGS[@]}" >/tmp/zeromesh-offload-render.log 2>&1 &
RENDER_PID=$!
echo "offload: render pid $RENDER_PID  log /tmp/zeromesh-offload-render.log"
echo "offload: monitor (q / Ctrl-C). This laptop stays free of chunk work."

trap 'kill -0 "$RENDER_PID" 2>/dev/null && echo "offload: render still running (pid $RENDER_PID)"' EXIT
exec "$ZM" monitor
