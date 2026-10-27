#!/usr/bin/env bash
# Big offload load for the pitch: 500 frames, remote-only, wall-clock timed.
#
#   ./scripts/demo_load.sh
#   ./scripts/demo_load.sh --frames 1-500 --chunk 16
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/scripts/demo_env.sh"

FRAMES="${FRAMES:-1-500}"
CHUNK="${CHUNK:-16}"
OUT="${OUT:-/tmp/zeromesh-demo-500}"
SCENE="${SCENE:-$ROOT/apps/zm-render/tests/data/spin_cube.blend}"
LOG="${LOG:-/tmp/zeromesh-demo-500.log}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --frames) FRAMES="$2"; shift 2 ;;
    --chunk) CHUNK="$2"; shift 2 ;;
    -o|--output) OUT="$2"; shift 2 ;;
    --scene) SCENE="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: demo_load.sh [--frames 1-500] [--chunk 16] [-o DIR]"
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
ZM_RENDER="$(command -v zm-render || true)"
[[ -n "$ZM" ]] || { echo "need zm"; exit 1; }
[[ -n "$ZM_RENDER" ]] || { echo "need zm-render"; exit 1; }
[[ -f "$SCENE" ]] || { echo "missing scene $SCENE"; exit 1; }

echo "=== peers (want donors ADMITTED) ==="
"$ZM" status || true
echo
echo "=== offload $FRAMES chunk=$CHUNK → $OUT (remote-only) ==="
mkdir -p "$OUT"
rm -f "$OUT"/frame_*.png 2>/dev/null || true

START_EPOCH=$(date +%s)
START_HMS=$(date -Is)
{
  echo "start: $START_HMS"
  echo "cmd: zm-render $SCENE --frames $FRAMES --chunk $CHUNK -o $OUT --remote-only"
} | tee "$LOG"

# /usr/bin/time -f for clear wall/user/sys; also keep zm-render stdout
 /usr/bin/time -f 'wall_sec=%e user_sec=%U sys_sec=%S' \
  -o "$LOG" -a \
  "$ZM_RENDER" "$SCENE" --frames "$FRAMES" --chunk "$CHUNK" -o "$OUT" --remote-only \
  2>&1 | tee -a "$LOG"

END_EPOCH=$(date +%s)
END_HMS=$(date -Is)
ELAPSED=$((END_EPOCH - START_EPOCH))
FRAMES_OK=$(ls -1 "$OUT"/frame_*.png 2>/dev/null | wc -l)

{
  echo "end: $END_HMS"
  echo "elapsed_sec=$ELAPSED"
  echo "frames_written=$FRAMES_OK"
} | tee -a "$LOG"

echo
echo "=== done ==="
echo "elapsed: ${ELAPSED}s   frames: $FRAMES_OK   log: $LOG"
echo "open monitor in another pane:  $ZM monitor"
