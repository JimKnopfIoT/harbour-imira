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
WPALOG=/tmp/imira-wpa.log
STATUS=/tmp/imira-status
CLOG=/tmp/imira-connect.log
PLOG=/tmp/imira-proto.log
DEBUGF=/tmp/imira-debug        # von der App (Diagnose-Seite): Supplicant mit -d
IFACE=""
ATTEMPTS=0

# Sekunden seit Boot. Kein $SECONDS: /bin/bash ist auf Serien-Telefonen
# busybox ash, das $SECONDS nicht kennt (mit set -u bricht das Skript ab).
# Nur Entwicklergeräte mit gnu-bash hatten es.
uptime_s() { cut -d. -f1 /proc/uptime; }
T_START=$(uptime_s)

status() { echo "$1 ${2:-0} ${ATTEMPTS} ${IFACE:--}" > "$STATUS"; }

# Protokoll für Fremd-Berichte: englisch, mit Datum/Uhrzeit.
clog() { echo "$(date '+%F %T') $*" >> "$CLOG"; }

cap_log() {
    # /tmp liegt im RAM: Logs auf die letzten $2 Bytes kürzen.
    [ -f "$1" ] || return 0
    [ "$(stat -c %s "$1" 2>/dev/null || echo 0)" -gt "$2" ] || return 0
    tail -c "$2" "$1" > "$1.tmp" && mv "$1.tmp" "$1"
}

radio_off() {
    # WLAN-Schalter der Einstellungen = rfkill über den ganzen Chip.
    for r in /sys/class/rfkill/rfkill*; do
        [ "$(cat "$r/type" 2>/dev/null)" = "wlan" ] || continue
        [ "$(cat "$r/soft" 2>/dev/null)" = "1" ] && return 0
        [ "$(cat "$r/state" 2>/dev/null)" = "0" ] && return 0
    done
    return 1
}

is_sink() {
    # wfd_subelems sieht so aus: <id:2><laenge:4><device-information:4>…
    # Die untersten zwei Bits der Device Information nennen den Gerätetyp:
    # 0 = Quelle, 1/2 = Senke, 3 = beides. Ohne diese Prüfung gilt jedes
    # Gerät mit WFD-Infos als Ziel — auch ein zweites Telefon mit imira,
    # das sich selbst als Quelle meldet.
    [ "${#1}" -ge 10 ] || return 1
    [ "${1:0:6}" = "000006" ] || return 1
    [ $(( 0x${1:6:4} & 3 )) -ne 0 ]
}

pick_iface() {
    # Standard: interner Chip (p2p0) — stabil und STA+P2P-fähig.
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

remove_stale_groups() {
    # Gruppen-Interfaces eines früheren Supplicants (p2p-p2p0-N), die bei
    # dessen Ende liegen geblieben sind. Der MediaTek-Treiber des J2 erlaubt
    # nur zwei zusätzliche Interfaces; sind sie belegt, scheitert jedes
    # connect sofort mit "Failed to create interface p2p-p2p0-0: -22".
    # Nur aufrufen, solange kein Supplicant von uns läuft.
    local d n
    for d in /sys/class/net/p2p-"$IFACE"-*; do
        [ -e "$d" ] || continue
        n=${d##*/}
        iw dev "$n" del 2>/dev/null && clog "removed leftover group interface $n"
    done
}

wpa_mode() { [ -e "$DEBUGF" ] && echo debug || echo normal; }

ensure_supplicant() {
    # Läuft er im falschen Log-Modus (Schalter seither umgelegt), neu starten.
    if [ "$(cat /var/run/imira-wpa.mode 2>/dev/null)" != "$(wpa_mode)" ]; then
        [ -f "$PIDF" ] && kill "$(cat "$PIDF")" 2>/dev/null
        rm -f "$PIDF"
        sleep 1
    fi
    if ! "$LIBEXEC/wpa_cli-p2p" -p "$CTRL" -i "$IFACE" ping >/dev/null 2>&1; then
        [ -f "$PIDF" ] && kill "$(cat "$PIDF")" 2>/dev/null
        sleep 1
        rm -f "$CTRL/$IFACE"
        mkdir -p /etc/imira
        sed "s/@DEVICE_NAME@/Sailfish/" "$LIBEXEC/wpa-imira.conf.in" > /etc/imira/wpa.conf
        # Die Gruppe läuft auf einem eigenen Interface, das wpa_supplicant
        # selbst anlegt (p2p-p2p0-0). Der MediaTek-Treiber des Jolla J2
        # nimmt auf den fest eingebauten p2p0/p2p1 gar kein NL80211_CMD_CONNECT
        # an (immer -22) — die Verbindung kommt dort nur über ein frisch
        # erzeugtes Interface zustande. Notausgang für Treiber, die keins
        # anlegen können: /etc/imira/no-group-iface anlegen.
        [ -e /etc/imira/no-group-iface ] && \
            echo "p2p_no_group_iface=1" >> /etc/imira/wpa.conf
        remove_stale_groups
        # Eigenes Logfile statt Syslog: journald ist auf manchen Geräten
        # winzig und ratenbegrenzt (J2: volatile, 1 MB, Burst 300) — dort
        # gingen Ereignisse wie P2P-GROUP-STARTED unbemerkt verloren.
        # Zwei Generationen aufheben: ein Scan nach einem gescheiterten Cast
        # startet einen neuen Supplicant — dessen Log darf den Befund nicht
        # überschreiben, sonst fehlt er im Diagnosebericht.
        [ -f "$WPALOG.1" ] && mv -f "$WPALOG.1" "$WPALOG.2"
        [ -s "$WPALOG" ] && mv -f "$WPALOG" "$WPALOG.1"
        : > "$WPALOG"
        # -t: Zeitstempel; -d nur auf Wunsch (Diagnose-Seite), dann stehen
        # auch die Begründungen drin (z.B. warum p2p_connect FAIL sagt).
        WPAOPT="-t"
        [ "$(wpa_mode)" = "debug" ] && WPAOPT="-t -d"
        "$LIBEXEC/wpa_supplicant-p2p" -Dnl80211 -i "$IFACE" -c /etc/imira/wpa.conf $WPAOPT \
            >> "$WPALOG" 2>&1 &
        echo $! > "$PIDF"
        wpa_mode > /var/run/imira-wpa.mode
        sleep 2
        W="$LIBEXEC/wpa_cli-p2p -p $CTRL -i $IFACE"
        $W ping >/dev/null 2>&1 || return 1
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
    grep -oE "[0-9]+ frames" "$PLOG" 2>/dev/null | tail -1 | cut -d" " -f1
}

peer_line() {
    # Wie imira-connect.sh: alles Diagnostisch-Relevante eines Peers auf
    # einer Zeile, ohne serial_number.
    $W p2p_peer "$1" 2>/dev/null | grep -E "^(device_name|manufacturer|model_name|model_number|pri_dev_type|config_methods|dev_capab|group_capab|flags|level|listen_freq|oper_freq|interface_addr|member_in_go_dev|wfd_subelems)=" \
        | tr '\n' ' '
}

net_check() {
    # Nach dem Gruppenstart: Liegt die Adresse richtig, führt die Route
    # zum Sink übers Gruppen-Interface, antwortet er überhaupt?
    clog "net: $(ip -4 -o addr show dev "$1" 2>/dev/null | awk '{print $2, $4}' | tr '\n' ' ')"
    clog "net: route to sink: $(ip route get "$2" 2>/dev/null | head -1)"
    if ping -c 1 -W 2 "$2" >/dev/null 2>&1; then
        clog "net: sink $2 answers ping"
    else
        clog "net: sink $2 does not answer ping (may just block ICMP)"
    fi
}

diag_scan() {
    # Aktive Prüfung für den Diagnosebericht: ~15 s nach Peers suchen und
    # alles festhalten, was jeder über sich verrät — auch wenn der Nutzer
    # nie bis zum Casten kommt.
    local out=/tmp/imira-diag-scan.log
    {
        echo "$(date '+%F %T') diagnostic scan on $IFACE"
        W="$LIBEXEC/wpa_cli-p2p -p $CTRL -i $IFACE"
        if ! ensure_supplicant; then
            echo "supplicant did not start"
        else
            echo "p2p status: $($W status 2>/dev/null | grep -E '^(p2p_state|wpa_state|p2p_device_address)=' | tr '\n' ' ')"
            $W p2p_find >/dev/null
            sleep 15
            N=0
            for a in $($W p2p_peers 2>/dev/null); do
                N=$((N + 1))
                SUB=$($W p2p_peer "$a" 2>/dev/null | grep -m1 "^wfd_subelems=" | cut -d= -f2-)
                if is_sink "$SUB"; then K=sink; else K=other; fi
                echo "peer $a [$K]: $(peer_line "$a")"
            done
            echo "$N peer(s) found"
            $W p2p_stop_find >/dev/null 2>&1
        fi
    } > "$out" 2>&1
    stop_supplicant
}

do_report() {
    # Diagnosebericht auf Wunsch der App. Die Anfrage-Datei enthält die
    # Optionen ("survey" = Funkumgebung einbeziehen).
    local opts
    opts=$(cat /tmp/imira-report-request 2>/dev/null)
    rm -f /tmp/imira-report-request /tmp/imira-report-done
    status reporting
    if radio_off; then
        echo "$(date '+%F %T') WLAN is off, no diagnostic scan" > /tmp/imira-diag-scan.log
    else
        pick_iface
        prep_iface
        diag_scan
    fi
    IMIRA_REPORT_OPTS="$opts" python3 "$LIBEXEC/imira-report.py" > /tmp/imira-report-done.new 2>/tmp/imira-report.err
    mv -f /tmp/imira-report-done.new /tmp/imira-report-done
    chmod 644 /tmp/imira-report-done
    # Das ausführliche Log war für diesen Bericht; danach wieder schlank.
    rm -f "$DEBUGF"
    status idle
}

scan_once() {
    W="$LIBEXEC/wpa_cli-p2p -p $CTRL -i $IFACE"
    $W p2p_find >/dev/null
    sleep 10
    : > /tmp/imira-devices.new
    for a in $($W p2p_peers 2>/dev/null); do
        INFO=$($W p2p_peer "$a" 2>/dev/null)
        NAME=$(echo "$INFO" | grep -m1 "^device_name=" | cut -d= -f2-)
        SUB=$(echo "$INFO" | grep -m1 "^wfd_subelems=" | cut -d= -f2-)
        if is_sink "$SUB"; then WFD=1; else WFD=0; fi
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
        if app_gone && [ $(($(uptime_s) - T_START)) -gt 15 ]; then
            exit 0
        fi
        # Bericht auch bei ausgeschaltetem WLAN (dann ohne Scan).
        if [ -e /tmp/imira-report-request ]; then
            do_report
            continue
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
    rm -f /tmp/imira-stop /tmp/imira-target /tmp/imira-radio
    ATTEMPTS=0
    pick_iface
    prep_iface
    status starting
    cap_log "$CLOG" 262144
    clog "=== cast start: imira $(rpm -q --qf '%{VERSION}-%{RELEASE}' harbour-imira 2>/dev/null)," \
         "iface $IFACE, mode $(cat /tmp/imira-mode 2>/dev/null || echo mirror)," \
         "res $(cat /tmp/imira-res 2>/dev/null || echo 1080), wpa log $(wpa_mode)," \
         "peer $(cat /tmp/imira-peer 2>/dev/null || echo auto)"
    if ! ensure_supplicant; then
        clog "wpa_supplicant did not start on $IFACE"
        status error
        sleep 5
        continue
    fi

    # Session-Schleife: läuft, bis Stop-Flag, App-Ende oder zu viele
    # Fehlversuche in Folge (kein endloses unsichtbares Wiederverbinden).
    FAILS=0
    while [ -e /tmp/imira-start ] && [ ! -e /tmp/imira-stop ]; do
        if app_gone; then
            clog "app closed, stopping session"
            break
        fi
        if [ "$FAILS" -ge 3 ]; then
            clog "3 failed attempts in a row, giving up"
            break
        fi
        for d in /proc/[0-9]*; do
            C=$(cat "$d/comm" 2>/dev/null)
            { [ "$C" = "imira-castd" ] || [ "$C" = "imira-comp" ]; } && kill -9 "${d#/proc/}" 2>/dev/null
        done
        W="$LIBEXEC/wpa_cli-p2p -p $CTRL -i $IFACE"
        # "*" statt des Interface-Namens: die Gruppe hängt an einem eigenen
        # Interface, dessen Name erst im Ereignis steht.
        $W p2p_group_remove '*' >/dev/null 2>&1
        status connecting
        # Frühere Handshakes dieser Sitzung nicht wegwerfen — sie landen in
        # der Historie (Diagnosebericht), proto.log selbst bleibt pro Versuch
        # (frames_of liest nur den laufenden).
        if [ -s "$PLOG" ]; then
            cat "$PLOG" >> /tmp/imira-proto.prev.log
            cap_log /tmp/imira-proto.prev.log 262144
        fi
        : > "$PLOG"
        ATTEMPTS=$((ATTEMPTS + 1))
        clog "session attempt $ATTEMPTS"
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
        IPS=$(IMIRA_IFACE="$IFACE" IMIRA_CTRL="$CTRL" IMIRA_WPA_LOG="$WPALOG" \
              IMIRA_ATTEMPTS=4 IMIRA_PEER="$PEER" IMIRA_FREQ="$FREQ" \
              "$LIBEXEC/imira-connect.sh" 2>>"$CLOG")
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
        PFX=${4:-24}
        # Die Sitzungsadresse gehört ausschließlich auf das Gruppen-Interface.
        # Ein Rest von früher — etwa auf p2p0, als die Gruppe noch dort lief —
        # kapert sonst die Route auf ein totes Interface: der Sink verbindet
        # sich, bekommt keine Antwort und wirft die Gruppe nach 15 s weg.
        [ "$IFACE" != "$GIF" ] && ip -4 addr flush dev "$IFACE" 2>/dev/null
        ip -4 addr flush dev "$GIF" 2>/dev/null
        ip addr add "$MY/$PFX" dev "$GIF" 2>/dev/null
        net_check "$GIF" "$GO"
        IMIRA_LOCAL_IP="$MY" IMIRA_SINK_IP="$GO" \
        IMIRA_STREAM_CMD="$LIBEXEC/run-castd.sh {ip} {port}" \
            python3 "$LIBEXEC/imira-wfd-proto.py" >> "$PLOG" 2>&1 &
        PROTO=$!
        # "streaming" erst, wenn wirklich Bilder rausgehen (castd meldet
        # "N frames" erst nach PLAY). Vorher steht die Gruppe zwar, aber
        # der Sink hat die Sitzung noch nicht aufgebaut — früher hieß das
        # schon "streaming", und der Bildschirm blieb schwarz.
        T0=$(uptime_s)
        NEIGH_LOGGED=0
        F=0
        while kill -0 "$PROTO" 2>/dev/null; do
            [ -e /tmp/imira-stop ] && break
            app_gone && break
            F=$(frames_of)
            if [ "${F:-0}" -gt 0 ]; then
                status streaming "$F"
            else
                status handshake
                if [ "$NEIGH_LOGGED" = 0 ] && [ $(($(uptime_s) - T0)) -ge 15 ] \
                        && ! grep -q "sink connected" "$PLOG"; then
                    # Hat der Sink uns wenigstens per ARP gesucht? Dann kennt
                    # er eine Adresse, erreicht aber den RTSP-Port nicht.
                    NEIGH_LOGGED=1
                    clog "no RTSP connection from the sink after 15 s"
                    clog "net: neighbours on $GIF: $(ip neigh show dev "$GIF" 2>/dev/null | tr '\n' ';')"
                fi
            fi
            sleep 2
        done
        kill "$PROTO" 2>/dev/null
        # castd meldet den Zähler nur alle 5 s — nach dem Schleifenende noch
        # einmal lesen, sonst stehen kurze Sitzungen mit 0 Bildern im Log.
        F=$(frames_of)
        clog "session attempt $ATTEMPTS ended after $(($(uptime_s) - T0)) s, ${F:-0} frames sent"
        # Ohne ein einziges gesendetes Bild zählt die Sitzung als Fehlversuch —
        # sonst verbindet sie endlos neu, ohne dass je etwas ankommt.
        if [ "${F:-0}" -eq 0 ] && [ ! -e /tmp/imira-stop ] && ! app_gone; then
            FAILS=$((FAILS + 1))
            status error
        fi
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
    "$LIBEXEC/wpa_cli-p2p" -p "$CTRL" -i "$IFACE" p2p_group_remove '*' >/dev/null 2>&1
    stop_supplicant
    clog "=== cast stopped"
    rm -f /tmp/imira-start /tmp/imira-target /tmp/imira-radio
    status idle
done
