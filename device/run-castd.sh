#!/bin/bash
# Startet den Streaming-Daemon als Session-User (Wayland-Zugriff) mit
# nacktem argv[0], damit busybox pgrep/pkill -x greifen.
# Auflösung/Bitrate kommen aus der Session-Umgebung (App-Einstellung).
exec su defaultuser -s /bin/bash -c "XDG_RUNTIME_DIR=/run/user/100000 WAYLAND_DISPLAY=../../display/wayland-0 exec -a imira-castd /usr/libexec/imira/imira-castd --dest $1 --port $2 --width ${IMIRA_W:-1920} --height ${IMIRA_H:-1080} --bitrate ${IMIRA_BR:-8000000}"
