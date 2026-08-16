#!/bin/bash
# Startet die Streaming-Kette als Session-User (Wayland-Zugriff) mit
# nacktem argv[0], damit busybox pgrep/pkill -x greifen.
# Auflösung/Bitrate kommen aus der Session-Umgebung (App-Einstellung),
# ebenso die Betriebsart (IMIRA_MODE: leer = Spiegeln, "convergence" =
# eigener Compositor als virtueller TV-Bildschirm).
W=${IMIRA_W:-1920}; H=${IMIRA_H:-1080}; BR=${IMIRA_BR:-8000000}

if [ "$IMIRA_MODE" = "convergence" ]; then
    # Virtueller TV-Bildschirm: imira-comp rendert die Fenster-Fläche und
    # publiziert Frames per Shared Memory; castd liest sie (IMIRA_INPUT=shm)
    # statt des lipstick-recorders — das Phone wird nicht gespiegelt.
    su defaultuser -s /bin/bash -c "XDG_RUNTIME_DIR=/run/user/100000 WAYLAND_DISPLAY=../../display/wayland-0 exec -a imira-comp /usr/libexec/imira/imira-comp --width $W --height $H" > /tmp/imira-comp.log 2>&1 &
    COMP=$!
    su defaultuser -s /bin/bash -c "XDG_RUNTIME_DIR=/run/user/100000 IMIRA_INPUT=shm exec -a imira-castd /usr/libexec/imira/imira-castd --dest $1 --port $2 --width $W --height $H --bitrate $BR"
    kill $COMP 2>/dev/null
    exit 0
fi

exec su defaultuser -s /bin/bash -c "XDG_RUNTIME_DIR=/run/user/100000 WAYLAND_DISPLAY=../../display/wayland-0 exec -a imira-castd /usr/libexec/imira/imira-castd --dest $1 --port $2 --width $W --height $H --bitrate $BR"
