#!/usr/bin/env bash
set -euo pipefail

# ZeroMesh helper script to install and configure recc (Remote Execution Caching Compiler)

echo "=== ZeroMesh: recc Compiler Launcher Setup ==="

if command -v recc >/dev/null 2>&1; then
    echo "✔ recc is already installed: $(which recc)"
else
    echo "Installing recc via package manager..."
    if command -v apt-get >/dev/null 2>&1; then
        # Debian/Ubuntu do not currently ship `recc` in the main archive.
        if apt-cache show recc >/dev/null 2>&1; then
            sudo apt-get update && sudo apt-get install -y recc
        else
            echo "recc is not in apt on this distro."
            echo "Build/install from https://gitlab.com/BuildGrid/buildbox/buildbox (recc),"
            echo "or use a distro that packages it (Fedora/dnf, Homebrew, Arch AUR)."
            exit 1
        fi
    elif command -v dnf >/dev/null 2>&1; then
        sudo dnf install -y recc
    elif command -v pacman >/dev/null 2>&1; then
        if pacman -Si recc >/dev/null 2>&1; then
            sudo pacman -S --needed recc
        elif command -v yay >/dev/null 2>&1; then
            yay -S --needed recc
        else
            echo "recc is not in the configured pacman repositories. Install yay, then run: yay -S recc"
            exit 1
        fi
    elif command -v brew >/dev/null 2>&1; then
        brew install recc
    else
        echo "Please install recc manually from: https://gitlab.com/BuildGrid/buildbox/buildbox"
        exit 1
    fi
fi

# Configure environment exports for zero-config build offloading
echo ""
echo "Add the following environment variables to your ~/.bashrc or build session:"
echo "------------------------------------------------------------------------"
echo "export RECC_SERVER=127.0.0.1:8980"
echo "export RECC_CAS_SERVER=127.0.0.1:8980"
echo "export RECC_INSTANCE=default"
echo "export CMAKE_CXX_COMPILER_LAUNCHER=recc"
echo "export CMAKE_C_COMPILER_LAUNCHER=recc"
echo "------------------------------------------------------------------------"
echo ""
echo "✔ ZeroMesh environment ready!"
