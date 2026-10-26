#!/usr/bin/env bash
# Donor machine setup: Blender check → zmd → zm monitor.
# Run this on the OTHER laptops. The requester offloads FROM theirs.
#
#   ./scripts/setup_donor.sh
#   ./scripts/setup_donor.sh --join 'ZM-XXXX-…-XXC'
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/scripts/demo_env.sh"

JOIN_CODE=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --join) JOIN_CODE="$2"; shift 2 ;;
    -h|--help)
      cat <<'EOF'
Usage: setup_donor.sh [--join 'ZM-…']
  1) check blender on PATH
  2) start zmd
  3) optional team join
  4) zm monitor
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
die() { echo "donor: $*" >&2; exit 1; }

[[ -n "$ZM" && -n "$ZMD" ]] || die "need zm/zmd (build first: ninja -C build-verify)"

echo "donor: checking blender…"
if ! command -v blender >/dev/null 2>&1; then
  die "blender not on PATH — install Blender and ensure \`blender\` works in this shell"
fi
echo "donor: blender OK → $(command -v blender)"
blender --version 2>/dev/null | head -1 || true

export ZM_BUILD_DIR="${ZM_BUILD_DIR:-$(dirname "$ZM")}"

if ! "$ZM" status >/dev/null 2>&1; then
  echo "donor: starting zmd…"
  "$ZMD" >/tmp/zeromesh-donor-zmd.log 2>&1 &
  for _ in $(seq 1 40); do
    "$ZM" status >/dev/null 2>&1 && break
    sleep 0.25
  done
  "$ZM" status >/dev/null 2>&1 || die "zmd failed — see /tmp/zeromesh-donor-zmd.log"
else
  echo "donor: zmd already running"
fi

if [[ -n "$JOIN_CODE" ]]; then
  echo "donor: joining team…"
  "$ZM" team join "$JOIN_CODE"
fi

echo "donor: status"
"$ZM" status || true
echo "donor: monitor (q / Ctrl-C) — leave this up while the requester offloads"
exec "$ZM" monitor
