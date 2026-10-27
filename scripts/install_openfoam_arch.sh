#!/usr/bin/env bash
# Install the OpenFOAM Foundation edition used by the ZeroMesh CFD demo on Arch Linux.
# Requires an AUR helper (yay or paru) and runs it as the invoking user.

set -euo pipefail

if [[ ! -r /etc/arch-release ]]; then
    echo "This installer is for Arch Linux and Arch-based distributions." >&2
    exit 2
fi

if command -v icoFoam >/dev/null 2>&1 && command -v blockMesh >/dev/null 2>&1; then
    echo "OpenFOAM tools are already available in PATH."
else
    if command -v yay >/dev/null 2>&1; then
        yay -S --needed openfoam-org
    elif command -v paru >/dev/null 2>&1; then
        paru -S --needed openfoam-org
    else
        cat >&2 <<'EOF'
OpenFOAM Foundation is provided through the AUR as openfoam-org.
Install an AUR helper, then run this script again. For example:
  sudo pacman -S --needed base-devel git
  # install yay or paru using your preferred documented method
EOF
        exit 1
    fi
fi

bashrc=""
if pacman -Q openfoam-org >/dev/null 2>&1; then
    bashrc="$(pacman -Ql openfoam-org | awk '$2 ~ /\/etc\/bashrc$/ { print $2; exit }')"
fi

if [[ -z "$bashrc" || ! -f "$bashrc" ]]; then
    bashrc="$(find /opt -path '*/etc/bashrc' -type f -print -quit 2>/dev/null || true)"
fi

if [[ -z "$bashrc" || ! -f "$bashrc" ]]; then
    echo "OpenFOAM installed, but its etc/bashrc could not be located." >&2
    echo "Locate it with: pacman -Ql openfoam-org | grep '/etc/bashrc$'" >&2
    exit 1
fi

if ! bash -lc 'source "$1" && command -v icoFoam >/dev/null && command -v blockMesh >/dev/null' bash "$bashrc"; then
    echo "OpenFOAM setup did not expose icoFoam and blockMesh." >&2
    exit 1
fi

echo "OpenFOAM is ready. Use this with zm-cfd on this machine:"
printf '  --bashrc %q\n' "$bashrc"
echo "Verify: bash -lc 'source \"$bashrc\" && icoFoam -help'"
