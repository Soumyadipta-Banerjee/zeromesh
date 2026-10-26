#!/usr/bin/env bash
# Quick path — every requester step in order, then monitor.
#
# Donors first (on other machines):
#   ./scripts/setup_donor.sh --join '<code from step 3>'
#
# This laptop:
#   ./scripts/demo_quick.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/scripts/demo_env.sh"

FRAMES="${FRAMES:-1-48}"
CHUNK="${CHUNK:-8}"
OUT="${OUT:-/tmp/zeromesh-offload-renders}"
SCENE="${SCENE:-$ROOT/apps/zm-render/tests/data/spin_cube.blend}"

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
die() { echo "quick: $*" >&2; exit 1; }

[[ -n "$ZM" && -n "$ZMD" ]] || die "need zm/zmd"
[[ -n "$ZM_RENDER" ]] || die "need zm-render (pip install -e ./apps/zm-render)"
[[ -f "$SCENE" ]] || die "missing scene $SCENE"
command -v blender >/dev/null 2>&1 || die "blender not on PATH (donors need it too)"
export ZM_BUILD_DIR="${ZM_BUILD_DIR:-$(dirname "$ZM")}"

step() { echo; echo "=== $1 ==="; }

step "1/5 start zmd"
if ! "$ZM" status >/dev/null 2>&1; then
  "$ZMD" >/tmp/zeromesh-quick-zmd.log 2>&1 &
  for _ in $(seq 1 40); do
    "$ZM" status >/dev/null 2>&1 && break
    sleep 0.25
  done
  "$ZM" status >/dev/null 2>&1 || die "zmd failed — /tmp/zeromesh-quick-zmd.log"
else
  echo "already running"
fi

step "2/5 team (create if none)"
INFO="$("$ZM" team info 2>/dev/null || true)"
if echo "$INFO" | grep -qi 'not in a team'; then
  echo "creating team…"
  PHRASE="$("$ZM" team create)"
  echo "join phrase: $PHRASE"
else
  echo "$INFO"
  PHRASE="(already in a team — reuse the phrase you shared, or: zm team leave && zm team create)"
fi
echo
echo ">>> On each donor:  ./scripts/setup_donor.sh --join $PHRASE"
echo ">>> Press Enter when donors are up and ADMITTED…"
read -r _

step "3/5 peers"
"$ZM" status || true

step "4/5 offload render (remote-only)"
mkdir -p "$OUT"
echo "zm-render $SCENE --frames $FRAMES --chunk $CHUNK -o $OUT --remote-only"
"$ZM_RENDER" "$SCENE" --frames "$FRAMES" --chunk "$CHUNK" -o "$OUT" --remote-only \
  >/tmp/zeromesh-quick-render.log 2>&1 &
RENDER_PID=$!
echo "render pid $RENDER_PID  log /tmp/zeromesh-quick-render.log"

step "5/5 monitor (q / Ctrl-C)"
trap 'kill -0 "$RENDER_PID" 2>/dev/null && echo "quick: render still running (pid $RENDER_PID)"' EXIT
exec "$ZM" monitor
