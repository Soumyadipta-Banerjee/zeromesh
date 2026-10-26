#!/usr/bin/env bash
set -euo pipefail

export PATH="/usr/bin:$PATH"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/build}"

if [ ! -f "$BUILD_DIR/zmd" ] || [ ! -f "$BUILD_DIR/zm" ]; then
    echo "Error: Binaries not found in $BUILD_DIR. Please run 'make build' first." >&2
    exit 1
fi

PKG_DIR=$(mktemp -d -t zeromesh-deb-XXXXXX)
trap 'rm -rf "$PKG_DIR"' EXIT

echo "Staging Debian package into $PKG_DIR..."

DESTDIR="$PKG_DIR" ninja -C "$BUILD_DIR" install

if [ -d "$PKG_DIR/usr/local/bin" ]; then
    mkdir -p "$PKG_DIR/usr/bin"
    mv "$PKG_DIR/usr/local/bin/"* "$PKG_DIR/usr/bin/"
    rm -rf "$PKG_DIR/usr/local"
fi

mkdir -p "$PKG_DIR/DEBIAN"
mkdir -p "$PKG_DIR/usr/lib/systemd/user"
mkdir -p "$PKG_DIR/usr/share/doc/zeromesh"

cp "$SCRIPT_DIR/control" "$PKG_DIR/DEBIAN/control"
cp "$SCRIPT_DIR/postinst" "$PKG_DIR/DEBIAN/postinst"
chmod 0755 "$PKG_DIR/DEBIAN/postinst"

cp "$SCRIPT_DIR/../systemd/zmd.service" "$PKG_DIR/usr/lib/systemd/user/zmd.service"
cp "$REPO_ROOT/README.md" "$PKG_DIR/usr/share/doc/zeromesh/README.md"

OUTPUT_DEB="$REPO_ROOT/zeromesh_0.1.0_amd64.deb"
dpkg-deb --build --root-owner-group "$PKG_DIR" "$OUTPUT_DEB"

echo "Package created successfully: $OUTPUT_DEB"
dpkg-deb --info "$OUTPUT_DEB"
