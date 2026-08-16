#!/usr/bin/env bash
# Build imira-castd (aarch64) with the Sailfish Platform SDK — direct sb2
# compile, no packaging. Result: daemon/imira-castd
set -e
SDK="${SDK_ROOT:-$HOME/SailfishOS-Platform-SDK}"
TARGET="${SFOS_TARGET:-SailfishOS-5.0.0.62-aarch64}"
HERE="$(cd "$(dirname "$0")" && pwd)"

"$SDK/sdks/sfossdk/sdk-chroot" sb2 -t "$TARGET" bash -c "
set -e
cd '$HERE'
gcc -O2 -c protocol/lipstick-recorder-protocol.c -o /tmp/imira-lrp.o \
    \$(pkg-config --cflags wayland-client)
g++ -O2 -std=c++14 -Wall -fPIC -Iprotocol -Isrc \
    src/main.cpp src/convert.cpp src/encoder.cpp src/recorder.cpp \
    src/tsmux.cpp src/rtpsender.cpp src/orientation.cpp \
    src/audiocapture.cpp /tmp/imira-lrp.o \
    -o imira-castd \
    \$(pkg-config --cflags --libs wayland-client Qt5Sensors Qt5Core libpulse) \
    -ldroidmedia -ldl -lpthread
strip imira-castd
"
echo "OK: $HERE/imira-castd"
