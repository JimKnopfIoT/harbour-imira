#!/bin/bash
# imira-session.sh — System-Service (root): wartet auf das Start-Flag der App,
# baut die Wi-Fi-Direct-Verbindung auf (Alfa wlan1 bevorzugt, sonst interner
# Chip p2p0) und fährt die WFD-Session. Läuft dauerhaft; die UI-App steuert
# über Flag-Dateien:  /tmp/imira-start  /tmp/imira-stop
# Status für die UI:  /tmp/imira-status  ("state frames attempts iface")
set -u
LIBEXEC=/usr/libexec/imira
CTRL=/var/run/wpa_imira
PIDF=/var/run/imira-wpa.pid
STATUS=/tmp/imira-status
IFACE=""
ATTEMPTS=0

status() { echo "$1 ${2:-0} ${ATTEMPTS} ${IFACE:--}" > "$STATUS"; }

radio_off() {
    # WLAN-Schalter der Einstellungen = rfkill über den ganzen Chip.
    for r in /sys/class/rfkill/rfkill*; do
        [ "$(cat "$r/type" 2>/dev/null)" = "wlan" ] || continue
        [ "$(cat "$r/soft" 2>/dev/null)" = "1" ] && return 0
        [ "$(cat "$r/state" 2>/dev/null)" = "0" ] && return 0
    done
    return 1
}

pick_iface() {
    # Standard: interner Qualcomm-Chip (p2p0) — stabil und STA+P2P-fähig.
    # Der 8812au-Treiber einer externen Alfa hat reproduzierbar Kernel-
    # Panics in der P2P-Verhandlung ausgelöst; sie wird nur noch benutzt,
    # wenn das ausdrücklich verlangt ist (touch /etc/imira/prefer-alfa).
    IFACE=p2p0
    if [ -e /etc/imira/prefer-alfa ] && [ -d /sys/class/net/wlan1 ]; then
        PHY=$(cat /sys/class/net/wlan1/phy80211/name 2>/dev/null)
        if [ -n "$PHY" ] && iw phy "$PHY" info 2>/dev/null | grep -q "P2P-client"; then
            IFACE=wlan1
        fi
    fi
}

prep_iface() {
    if [ "$IFACE" = "wlan1" ]; then
        # Aus einem eventuellen Monitor-Mode (iwifi) zurückholen.
        ip link set wlan1 down 2>/dev/null
        iw dev wlan1 set type managed 2>/dev/null
        ip link set wlan1 up 2>/dev/null
    else
        # p2p0 liegt nach dem Boot down — ohne UP scannt es stumm ins Leere.
        ip link set p2p0 up 2>/dev/null
    fi
}

ensure_supplicant() {
    if ! "$LIBEXEC/wpa_cli-p2p" -p "$CTRL" -i "$IFACE" ping >/dev/null 2>&1; then
        [ -f "$PIDF" ] && kill "$(cat "$PIDF")" 2>/dev/null
        sleep 1
        mkdir -p /etc/imira
        sed "s/@DEVICE_NAME@/Sailfish/" "$LIBEXEC/wpa-imira.conf.in" > /etc/imira/wpa.conf
        "$LIBEXEC/wpa_supplicant-p2p" -Dnl80211 -i "$IFACE" -c /etc/imira/wpa.conf \
            -B -s -P "$PIDF" || return 1
        sleep 2
        W="$LIBEXEC/wpa_cli-p2p -p $CTRL -i $IFACE"
        $W set wifi_display 1 >/dev/null
        # WFD-IE: Source, RTSP-Port 7236, 50 Mbit/s
        $W wfd_subelem_set 0 000600101c440032 >/dev/null
    fi
    return 0
}

stop_supplicant() {
    # Im Ruhezustand gehört der Funk ungeteilt dem normalen WLAN — unser
    # P2P-Supplicant läuft nur, solange Scan oder Session ihn brauchen.
    [ -f "$PIDF" ] && kill "$(cat "$PIDF")" 2>/dev/null
    rm -f "$PIDF"
}

recover_iface() {
    # Der 8812au-Treiber der Alfa verliert nach Modewechseln gern den Scan —
    # Modul-Reload + frischer Supplicant beheben das zuverlässig.
    [ "$IFACE" = "wlan1" ] || return 0
    [ -f "$PIDF" ] && kill "$(cat "$PIDF")" 2>/dev/null
    sleep 1
    ip link set wlan1 down 2>/dev/null
    rmmod 8812au 2>/dev/null
    sleep 2
    modprobe 8812au 2>/dev/null
    sleep 4
    prep_iface
    ensure_supplicant
}

app_gone() {
    # App geschlossen/abgestürzt = Heartbeat älter als 10 s → Session beenden
    # (Nutzerentscheidung: ohne App keine Übertragung).
    [ -e /tmp/imira-app-alive ] || return 0
    local now mtime
    now=$(date +%s)
    mtime=$(stat -c %Y /tmp/imira-app-alive 2>/dev/null || echo 0)
    [ $((now - mtime)) -gt 10 ]
}

frames_of() {
    # Frame-Zähler aus dem castd-Log der laufenden Session ziehen.
    grep -oE "[0-9]+ frames" /tmp/imira-proto.log 2>/dev/null | tail -1 | cut -d" " -f1
}

scan_once() {
    W="$LIBEXEC/wpa_cli-p2p -p $CTRL -i $IFACE"
    $W p2p_find >/dev/null
    sleep 10
    : > /tmp/imira-devices.new
    for a in $($W p2p_peers 2>/dev/null); do
        INFO=$($W p2p_peer "$a" 2>/dev/null)
        NAME=$(echo "$INFO" | grep -m1 "^device_name=" | cut -d= -f2-)
        if echo "$INFO" | grep -q "^wfd_subelems="; then WFD=1; else WFD=0; fi
        printf '%s\t%s\t%s\n' "$a" "$WFD" "${NAME:-?}" >> /tmp/imira-devices.new
    done
    $W p2p_stop_find >/dev/null 2>&1
    [ -s /tmp/imira-devices.new ]
}

do_scan() {
    # Auf UI-Wunsch nach Miracast-Empfängern suchen; Ergebnis für die App
    # nach /tmp/imira-devices ("mac<TAB>wfd<TAB>name", wfd=1 → echter Sink).
    pick_iface
    prep_iface
    status scanning
    if ! ensure_supplicant; then
        status error
        return
    fi
    if ! scan_once; then
        # Leerer Scan = fast immer die eingeschlafene Alfa. Einmal heilen
        # und wiederholen, bevor wir eine leere Liste abliefern.
        recover_iface
        status scanning
        scan_once || true
    fi
    mv /tmp/imira-devices.new /tmp/imira-devices
    chmod 644 /tmp/imira-devices
    stop_supplicant
    status idle
}

# Frischer Service-Start = neutraler Zustand. Ein übrig gebliebenes
# Start-Flag (Update, Reboot, Absturz) darf NIE von selbst verbinden —
# übertragen wird erst, wenn der Button in der App frisch gedrückt wird.
rm -f /tmp/imira-start /tmp/imira-stop /tmp/imira-scan

status idle
while true; do
    if [ ! -e /tmp/imira-start ]; then
        # App weg -> Selbstbeendung (nur im Leerlauf; 15 s Anlaufgnade).
        if app_gone && [ "$SECONDS" -gt 15 ]; then
            exit 0
        fi
        if radio_off; then
            status nowlan
            rm -f /tmp/imira-scan
            sleep 2
            continue
        fi
        [ "$(cut -d" " -f1 "$STATUS" 2>/dev/null)" = "nowlan" ] && status idle
        if [ -e /tmp/imira-scan ]; then
            rm -f /tmp/imira-scan
            do_scan
        fi
        sleep 2
        continue
    fi
    if radio_off; then
        status nowlan
        sleep 2
        continue
    fi
    rm -f /tmp/imira-stop /tmp/imira-target
    ATTEMPTS=0
    pick_iface
    prep_iface
    status starting
    if ! ensure_supplicant; then
        status error
        sleep 5
        continue
    fi

    # Session-Schleife: läuft, bis Stop-Flag, App-Ende oder zu viele
    # Fehlversuche in Folge (kein endloses unsichtbares Wiederverbinden).
    FAILS=0
    while [ -e /tmp/imira-start ] && [ ! -e /tmp/imira-stop ]; do
        if app_gone; then
            echo "app beendet — session wird gestoppt" >> /tmp/imira-connect.log
            break
        fi
        if [ "$FAILS" -ge 3 ]; then
            echo "3 Fehlversuche in Folge — gebe auf" >> /tmp/imira-connect.log
            break
        fi
        for d in /proc/[0-9]*; do
            C=$(cat "$d/comm" 2>/dev/null)
            { [ "$C" = "imira-castd" ] || [ "$C" = "imira-comp" ]; } && kill -9 "${d#/proc/}" 2>/dev/null
        done
        W="$LIBEXEC/wpa_cli-p2p -p $CTRL -i $IFACE"
        $W p2p_group_remove "$IFACE" >/dev/null 2>&1
        status connecting
        : > /tmp/imira-proto.log
        ATTEMPTS=$((ATTEMPTS + 1))
        # Auflösung aus der App-Einstellung (wirkt pro Session): 720 oder 1080.
        RES=$(cat /tmp/imira-res 2>/dev/null)
        if [ "$RES" = "720" ]; then
            IMIRA_CEA=00000020; IMIRA_W=1280; IMIRA_H=720; IMIRA_BR=5000000
        else
            IMIRA_CEA=00000080; IMIRA_W=1920; IMIRA_H=1080; IMIRA_BR=8000000
        fi
        # Betriebsart aus der App: "convergence" = eigener Compositor als
        # virtueller TV-Bildschirm; alles andere/keine Datei = Spiegeln.
        IMIRA_MODE=$(cat /tmp/imira-mode 2>/dev/null)
        export IMIRA_CEA IMIRA_W IMIRA_H IMIRA_BR IMIRA_MODE
        PEER=$(cat /tmp/imira-peer 2>/dev/null)
        FREQ=""
        [ "$IFACE" = "wlan1" ] && FREQ=2437   # nur die Alfa braucht den Zwang
        IPS=$(IMIRA_IFACE="$IFACE" IMIRA_CTRL="$CTRL" IMIRA_WPA_PID="$PIDF" \
              IMIRA_ATTEMPTS=4 IMIRA_PEER="$PEER" IMIRA_FREQ="$FREQ" \
              "$LIBEXEC/imira-connect.sh" 2>>/tmp/imira-connect.log)
        if [ -z "$IPS" ]; then
            FAILS=$((FAILS + 1))
            status error
            recover_iface
            sleep 3
            continue
        fi
        FAILS=0
        set -- $IPS
        GIF=$1
        MY=$2
        GO=$3
        ip addr add "$MY/24" dev "$GIF" 2>/dev/null
        IMIRA_LOCAL_IP="$MY" IMIRA_SINK_IP="$GO" \
        IMIRA_STREAM_CMD="$LIBEXEC/run-castd.sh {ip} {port}" \
            python3 "$LIBEXEC/imira-wfd-proto.py" >> /tmp/imira-proto.log 2>&1 &
        PROTO=$!
        status streaming
        while kill -0 "$PROTO" 2>/dev/null; do
            [ -e /tmp/imira-stop ] && break
            app_gone && break
            status streaming "$(frames_of)"
            sleep 2
        done
        kill "$PROTO" 2>/dev/null
        sleep 2
    done

    # Aufräumen nach Stop.
    for d in /proc/[0-9]*; do
        C=$(cat "$d/comm" 2>/dev/null)
        { [ "$C" = "imira-castd" ] || [ "$C" = "imira-comp" ]; } && kill -9 "${d#/proc/}" 2>/dev/null
    done
    # Falls der Daemon hart starb, bevor er seine Stille-Senke entladen
    # konnte: Modul anhand des Sink-Namens finden und entladen, sonst
    # bleiben die Medien-Streams stumm geparkt.
    MID=$(su defaultuser -s /bin/sh -c "XDG_RUNTIME_DIR=/run/user/100000 pactl list short modules" 2>/dev/null \
          | grep "sink_name=imira_cast" | cut -f1)
    [ -n "$MID" ] && su defaultuser -s /bin/sh -c "XDG_RUNTIME_DIR=/run/user/100000 pactl unload-module $MID" 2>/dev/null
    "$LIBEXEC/wpa_cli-p2p" -p "$CTRL" -i "$IFACE" p2p_group_remove "${GIF:-$IFACE}" >/dev/null 2>&1
    "$LIBEXEC/wpa_cli-p2p" -p "$CTRL" -i "$IFACE" p2p_group_remove "$IFACE" >/dev/null 2>&1
    stop_supplicant
    rm -f /tmp/imira-start /tmp/imira-target
    status idle
done
