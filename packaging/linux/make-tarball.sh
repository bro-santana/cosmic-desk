#!/bin/sh
# Cosmic Desk - Linux tarball packaging (plan M6.3).
#
# Usage (from anywhere, after `cmake -B build && cmake --build build`):
#   packaging/linux/make-tarball.sh
#
# Installs the CMake install rules (lib/cosmicdesk/cosmicdesk + assets,
# share/applications/cosmicdesk.desktop, share/cosmicdesk, share/icons/...)
# into a staging prefix and tars it. The udev rule is installed into
# share/cosmicdesk but is NOT activated - users copy it to
# /etc/udev/rules.d/ themselves.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
STAGE="$ROOT/build/dist-linux/CosmicDesk"
rm -rf "$STAGE"
cmake --install "$ROOT/build" --prefix "$STAGE"
# assets are installed by cmake; nothing extra needed here
tar -C "$ROOT/build/dist-linux" -czf "$ROOT/build/dist-linux/cosmicdesk-linux-x64.tar.gz" CosmicDesk
echo "Tarball: $ROOT/build/dist-linux/cosmicdesk-linux-x64.tar.gz"
echo "Install the udev rule manually: sudo cp share/cosmicdesk/60-cosmicdesk-input.rules /etc/udev/rules.d/"