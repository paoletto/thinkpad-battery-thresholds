#!/bin/bash
# Build and install with setuid root so regular user can write sysfs thresholds.
set -e

if [ $# -lt 1 ] || [ -z "$1" ]; then
    echo "Usage: $0 <install-path>" >&2
    echo "Example: $0 /usr/local/bin/thresholdctl-gui" >&2
    exit 1
fi
DEST="$1"

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SRC_DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"
if [ -z "$QMAKE" ]; then
    echo "qmake not found. Install qt6-base-dev (Debian) or qt6-base (Arch)." >&2
    exit 1
fi

"$QMAKE" "$SRC_DIR/thinkpad-battery-thresholds.pro"
make -j"$(nproc)"

sudo install -o root -g root -m 4755 thresholdctl-gui "$DEST"
echo "Installed to $DEST with setuid bit."

DESKTOP_SRC="$SRC_DIR/thresholdctl-gui.desktop"
if [ -f "$DESKTOP_SRC" ]; then
    sudo install -m 0644 "$DESKTOP_SRC" /usr/share/applications/thresholdctl-gui.desktop
    echo "Installed desktop entry."
fi
