#!/usr/bin/env bash
# Install the Ubuntu OpenFOAM package used by the ZeroMesh CFD demo.

set -euo pipefail

if ! command -v apt-get >/dev/null 2>&1; then
    echo "This installer requires Ubuntu or another apt-based distribution." >&2
    exit 2
fi

if ! dpkg -s openfoam >/dev/null 2>&1; then
    sudo apt-get update
    sudo apt-get install -y openfoam
fi

bashrc="$(dpkg -L openfoam | awk '$0 ~ /\/etc\/bashrc$/ { print; exit }')"
if [[ -z "$bashrc" || ! -f "$bashrc" ]]; then
    bashrc="$(find /usr/lib/openfoam /opt -path '*/etc/bashrc' -type f -print -quit 2>/dev/null || true)"
fi

if [[ -z "$bashrc" || ! -f "$bashrc" ]]; then
    echo "OpenFOAM installed, but its etc/bashrc could not be located." >&2
    echo "Locate it with: dpkg -L openfoam | grep '/etc/bashrc$'" >&2
    exit 1
fi

if ! bash -lc 'source "$1" && command -v icoFoam >/dev/null && command -v blockMesh >/dev/null' bash "$bashrc"; then
    echo "OpenFOAM setup did not expose icoFoam and blockMesh." >&2
    exit 1
fi

echo "OpenFOAM is ready. Use this with zm-cfd on this machine:"
printf '  --bashrc %q\n' "$bashrc"
echo "Verify: bash -lc 'source \"$bashrc\" && icoFoam -help'"
