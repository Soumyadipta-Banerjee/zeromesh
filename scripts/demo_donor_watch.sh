#!/usr/bin/env bash
# Run on a DONOR (bb) in a second terminal while the requester runs zm-render.
# Shows live Blender workers: CPU + frame range from argv (-s / -e).
set -euo pipefail
echo "Donor watch — Ctrl-C to stop"
echo "Looking for blender processes (ZeroMesh sandbox tasks)…"
echo
while true; do
  clear
  date '+%H:%M:%S'
  echo "=== this host: $(hostname) ==="
  echo
  # shellcheck disable=SC2009
  found=0
  while IFS= read -r line; do
    found=1
    pid=$(awk '{print $2}' <<<"$line")
    cpu=$(ps -o pcpu= -p "$pid" 2>/dev/null | tr -d ' ' || echo "?")
    et=$(ps -o etime= -p "$pid" 2>/dev/null | tr -d ' ' || echo "?")
    cmd=$(tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null || true)
    # Extract -s N -e M if present
    s=$(grep -oE '\-s [0-9]+' <<<"$cmd" | awk '{print $2}' | head -1)
    e=$(grep -oE '\-e [0-9]+' <<<"$cmd" | awk '{print $2}' | head -1)
    if [[ -n "${s:-}" && -n "${e:-}" ]]; then
      echo "▶  PID $pid  CPU ${cpu}%  time $et  RENDERING frames ${s}-${e}"
    else
      echo "▶  PID $pid  CPU ${cpu}%  time $et"
    fi
    echo "   $cmd" | fold -s -w 100 | sed 's/^/   /'
    echo
  done < <(ps -eo pid,cmd | grep -E '[b]lender' || true)

  if [[ "$found" -eq 0 ]]; then
    echo "(idle — no blender task right now)"
    echo "When zm-render runs on the requester, chunks show up here."
  fi
  echo
  echo "Tip: also run  ./build/zm monitor  for peer/load view"
  sleep 0.5
done
