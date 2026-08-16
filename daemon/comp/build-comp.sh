#!/usr/bin/env bash
# Build imira-comp (aarch64) with the Sailfish Platform SDK — direct sb2
# compile, no packaging. Result: daemon/comp/imira-comp
set -e
SDK="${SDK_ROOT:-$HOME/SailfishOS-Platform-SDK}"
TARGET="${SFOS_TARGET:-SailfishOS-5.0.0.62-aarch64}"
HERE="$(cd "$(dirname "$0")" && pwd)"

"$SDK/sdks/sfossdk/sdk-chroot" sb2 -t "$TARGET" bash -c "
set -e
cd '$HERE'
g++ -O2 -std=c++14 -Wall -fPIC \
    imira-comp.cpp \
    -o imira-comp \
    \$(pkg-config --cflags --libs Qt5Compositor Qt5Quick Qt5Gui Qt5Core) \
    -lrt -lGLESv2
strip imira-comp
"
echo "OK: $HERE/imira-comp"
