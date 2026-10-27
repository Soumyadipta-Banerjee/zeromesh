#!/usr/bin/env bash
# One-shot donor: blender → env → build → zmd → monitor.
#
#   bash ./scripts/donor_step.sh
#   bash ./scripts/donor_step.sh --join 'ZM-XXXX-…-XXC'
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

JOIN_CODE=""
JOBS="${JOBS:-2}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --join) JOIN_CODE="$2"; shift 2 ;;
    -j|--jobs) JOBS="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: donor_step.sh [--join 'ZM-…'] [-j N]"
      exit 0
      ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

die() { echo "donor: $*" >&2; exit 1; }
step() { echo; echo "=== $1 ==="; }

step "1/5 branch demo/m5-all"
git fetch origin demo/m5-all 2>/dev/null || git fetch origin
git checkout demo/m5-all
git pull --ff-only origin demo/m5-all || true

step "2/5 blender + demo env"
# shellcheck disable=SC1091
source "$ROOT/scripts/demo_env.sh"
command -v blender >/dev/null 2>&1 || die "install Blender so \`command -v blender\` resolves"
# Prefer the real binary path (which/command -v → realpath).
BLENDER_BIN="$(command -v blender)"
if command -v realpath >/dev/null 2>&1; then
  BLENDER_BIN="$(realpath "$BLENDER_BIN")"
fi
[[ -x "$BLENDER_BIN" ]] || die "blender not executable: $BLENDER_BIN"
echo "blender → $BLENDER_BIN"
# zmd env-host bare-name PATH is only /usr/local/bin:/usr/bin:/bin.
# Absolute /opt/... paths are fine when the requester passes --blender <path>.
# Also link a sandbox-visible name so bare \`blender\` works.
case "$BLENDER_BIN" in
  /usr/local/bin/blender|/usr/bin/blender|/bin/blender) ;;
  *)
    if [[ -w /usr/local/bin ]] || sudo -n true 2>/dev/null; then
      echo "linking $BLENDER_BIN → /usr/local/bin/blender (env-host PATH)"
      sudo ln -sfn "$BLENDER_BIN" /usr/local/bin/blender
    else
      die "need sandbox-visible blender — run: sudo ln -sfn $BLENDER_BIN /usr/local/bin/blender"
    fi
    ;;
esac
"$BLENDER_BIN" --version 2>/dev/null | head -1 || true

step "3/5 build zm / zmd"
BUILD=""
for d in build-verify build; do
  if [[ -x "$ROOT/$d/zm" && -x "$ROOT/$d/zmd" ]]; then BUILD="$ROOT/$d"; break; fi
done
if [[ -z "$BUILD" ]]; then
  BUILD="$ROOT/build"
  [ -f /tmp/zm-native.ini ] || {
    if [ -f "$HOME/zeromesh-backups/zm-native.ini" ]; then
      cp "$HOME/zeromesh-backups/zm-native.ini" /tmp/zm-native.ini
    else
      printf '%s\n' '[binaries]' 'c = '\''gcc'\''' 'cpp = '\''g++'\''' > /tmp/zm-native.ini
    fi
  }
  if [[ ! -f "$BUILD/build.ninja" ]]; then
    meson setup "$BUILD" --native-file /tmp/zm-native.ini
  fi
  ninja -C "$BUILD" -j"$JOBS" zm zmd
fi
ZM="$BUILD/zm"
ZMD="$BUILD/zmd"
[[ -x "$ZM" && -x "$ZMD" ]] || die "build failed — no $BUILD/zm or zmd"
export PATH="$BUILD:$PATH"
export ZM_BUILD_DIR="$BUILD"
echo "using $BUILD"

step "4/5 run zmd"
if ! "$ZM" status >/dev/null 2>&1; then
  "$ZMD" >/tmp/zeromesh-donor-zmd.log 2>&1 &
  for _ in $(seq 1 40); do
    "$ZM" status >/dev/null 2>&1 && break
    sleep 0.25
  done
  "$ZM" status >/dev/null 2>&1 || die "zmd failed — see /tmp/zeromesh-donor-zmd.log"
else
  echo "zmd already running"
fi

if [[ -n "$JOIN_CODE" ]]; then
  echo "joining team…"
  "$ZM" team join "$JOIN_CODE"
fi
"$ZM" status || true

echo
echo "requester tip:  zm-render … --remote-only --blender blender"
echo "               (donors need blender on /usr/local/bin|/usr/bin — linked above)"

step "5/5 zm monitor (q / Ctrl-C) — leave this up"
exec "$ZM" monitor
