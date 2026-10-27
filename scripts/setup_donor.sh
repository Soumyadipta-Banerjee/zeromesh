#!/usr/bin/env bash
# Donor machine — do these steps in order, then leave monitor open.
#
#   ./scripts/setup_donor.sh
#   ./scripts/setup_donor.sh --join 'ZM-XXXX-…-XXC'
#
# 1) checkout demo/m5-all + pull
# 2) blender + build env
# 3) build zm/zmd if missing
# 4) run zmd
# 5) zm monitor
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
      echo "Usage: setup_donor.sh [--join 'ZM-…'] [-j N]"
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
command -v blender >/dev/null 2>&1 || die "install Blender and put \`blender\` on PATH"
echo "blender → $(command -v blender)"
blender --version 2>/dev/null | head -1 || true

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
      # Minimal native file pointing at system tools; demo_env already set PKG_CONFIG_PATH.
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

step "5/5 zm monitor (q / Ctrl-C) — leave this up"
exec "$ZM" monitor
