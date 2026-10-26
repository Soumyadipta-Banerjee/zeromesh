#!/usr/bin/env bash
# Short Blender mesh demo: env → zmd → zm-render → zm monitor.
# Usage (from repo root or anywhere):
#   ./scripts/demo_blender.sh
#   ./scripts/demo_blender.sh --frames 1-48 --chunk 8
#   FRAMES=1-240 CHUNK=16 ./scripts/demo_blender.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/scripts/demo_env.sh"

FRAMES="${FRAMES:-1-48}"
CHUNK="${CHUNK:-8}"
OUT="${OUT:-/tmp/zeromesh-demo-renders}"
SCENE="${SCENE:-$ROOT/apps/zm-render/tests/data/spin_cube.blend}"
REMOTE_ONLY="${REMOTE_ONLY:-0}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --frames) FRAMES="$2"; shift 2 ;;
    --chunk) CHUNK="$2"; shift 2 ;;
    -o|--output) OUT="$2"; shift 2 ;;
    --scene) SCENE="$2"; shift 2 ;;
    --remote-only) REMOTE_ONLY=1; shift ;;
    -h|--help)
      cat <<'EOF'
Usage: demo_blender.sh [--frames A-B] [--chunk N] [-o DIR] [--scene FILE] [--remote-only]
Starts zmd if needed, runs zm-render in the background, then zm monitor.
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

if [[ -z "$ZM" || -z "$ZMD" ]]; then
  echo "demo: need zm/zmd (build with: meson setup build && ninja -C build)" >&2
  exit 1
fi
if [[ -z "$ZM_RENDER" ]]; then
  echo "demo: zm-render not on PATH (pip install -e ./apps/zm-render in the venv)" >&2
  exit 1
fi
if ! command -v blender >/dev/null 2>&1; then
  echo "demo: blender not on PATH" >&2
  exit 1
fi
if [[ ! -f "$SCENE" ]]; then
  echo "demo: scene not found: $SCENE" >&2
  exit 1
fi

mkdir -p "$OUT"
export ZM_BUILD_DIR="${ZM_BUILD_DIR:-$(dirname "$ZM")}"

if ! "$ZM" status >/dev/null 2>&1; then
  echo "demo: starting zmd…"
  "$ZMD" >/tmp/zeromesh-demo-zmd.log 2>&1 &
  for _ in $(seq 1 40); do
    if "$ZM" status >/dev/null 2>&1; then break; fi
    sleep 0.25
  done
  if ! "$ZM" status >/dev/null 2>&1; then
    echo "demo: zmd did not come up — see /tmp/zeromesh-demo-zmd.log" >&2
    exit 1
  fi
fi

echo "demo: peers"
"$ZM" status || true

RENDER_ARGS=(--frames "$FRAMES" --chunk "$CHUNK" -o "$OUT")
if [[ "$REMOTE_ONLY" == "1" ]]; then
  RENDER_ARGS+=(--remote-only)
fi

echo "demo: zm-render $SCENE ${RENDER_ARGS[*]}"
"$ZM_RENDER" "$SCENE" "${RENDER_ARGS[@]}" >/tmp/zeromesh-demo-render.log 2>&1 &
RENDER_PID=$!
echo "demo: render pid $RENDER_PID (log /tmp/zeromesh-demo-render.log)"
echo "demo: opening monitor — q or Ctrl-C to leave"

cleanup() {
  if kill -0 "$RENDER_PID" 2>/dev/null; then
    echo "demo: render still running (pid $RENDER_PID); leaving it"
  fi
}
trap cleanup EXIT

exec "$ZM" monitor
