#!/bin/bash
# imira-connect.sh — Wi-Fi-Direct-Verbindung zu einem WFD-Sink aufbauen.
# Der Sink muss Group Owner werden (Rollen-Verhandlung ist ein Münzwurf,
# daher Retry-Schleife). Erfolgreich, wenn P2P-GROUP-STARTED ... client.
#
# Env: IMIRA_IFACE (wlan1|p2p0), IMIRA_CTRL (ctrl-Socket-Dir),
#      IMIRA_WPA_LOG (Logdatei des Supplicants), IMIRA_PEER (optional MAC),
#      IMIRA_FREQ (default 2437), IMIRA_ATTEMPTS (default 12)
# Ausgabe bei Erfolg auf stdout: "<gruppen-iface> <eigene-ip> <go-ip> <prefix>".
# Protokoll (englisch, für Fremd-Berichte) auf stderr → /tmp/imira-connect.log.
set -u
IFACE="${IMIRA_IFACE:-wlan1}"
CTRL="${IMIRA_CTRL:-/var/run/wpa_imira}"
WPALOG="${IMIRA_WPA_LOG:-/tmp/imira-wpa.log}"
FREQ="${IMIRA_FREQ:-}"   # leer = Treiber/GO wählt den Kanal
MAX="${IMIRA_ATTEMPTS:-12}"
W="/usr/libexec/imira/wpa_cli-p2p -p $CTRL -i $IFACE"

log() { echo "$(date +%H:%M:%S) $*" >&2; }

peer_info() {
    # Was der Supplicant über den Peer weiß, auf einer Zeile. Genau das
    # braucht man bei einem fremden Sink: Hersteller/Modell, WPS-Methoden,
    # Capabilities, Flags (z.B. nur als Gruppen-Client gesehen) und die
    # WFD-Infos. serial_number bleibt draußen.
    $W p2p_peer "$1" 2>/dev/null | grep -E "^(device_name|manufacturer|model_name|model_number|pri_dev_type|config_methods|dev_capab|group_capab|flags|level|listen_freq|oper_freq|interface_addr|member_in_go_dev|wfd_subelems)=" \
        | tr '\n' ' '
}

log_events() {
    # Alle P2P/WPS-Ereignisse dieses Versuchs aus dem Supplicant-Log, ohne
    # das Discovery-Rauschen. Erklärt, WARUM ein Versuch scheiterte
    # (GO-NEG-FAILURE status=…, FORMATION-FAILURE, GROUP-REMOVED reason=…).
    sed -n "$((MARK + 1)),\$p" "$WPALOG" 2>/dev/null \
        | grep -E "(P2P|WPS|CTRL-EVENT)-[A-Z]|Failed|failed|rejected|refused" \
        | grep -vE "P2P-DEVICE-(FOUND|LOST)|P2P-FIND-STOPPED|CTRL-EVENT-(SCAN|BSS|REGDOM|NETWORK-NOT-FOUND)|WPS-AP-AVAILABLE" \
        | tail -40 | sed "s/^/    wpa: /" >&2
}

sta_freq() {
    # Kanal der normalen WLAN-Verbindung des Telefons (leer = nicht verbunden).
    iw dev wlan0 link 2>/dev/null | awk '/freq:/ {print int($2)}'
}

p2p_freq_ok() {
    # Darf Wi-Fi Direct auf dieser Frequenz eine Gruppe betreiben? 2,4 GHz
    # und die 5-GHz-Kanäle ohne Radarpflicht (36-48, 149-165); auf DFS-
    # Kanälen (52-144) darf kein P2P-Gerät von sich aus senden.
    local f=$1
    [ -n "$f" ] || return 1
    { [ "$f" -ge 2412 ] && [ "$f" -le 2472 ]; } && return 0
    { [ "$f" -ge 5180 ] && [ "$f" -le 5240 ]; } && return 0
    { [ "$f" -ge 5745 ] && [ "$f" -le 5825 ]; } && return 0
    return 1
}

dhcp_lease() {
    # Rückfall, wenn der Sink die Adresse nicht schon im Handshake vergibt
    # (IP-Vergabe per EAPOL gibt es erst seit P2P v1.3; der MS-Adapter kann
    # sie, viele TVs nicht). Ein GO betreibt dann einen DHCP-Server und
    # verbindet sich zu der Adresse, die er selbst verliehen hat — fragen
    # wir nicht, kennt er uns nicht und öffnet nie die RTSP-Verbindung.
    # udhcpc (busybox) konfiguriert nichts selbst; der Hook schreibt nur
    # "ip router serverid prefix lease" in eine Datei.
    local out=/tmp/imira-dhcp.lease
    rm -f "$out"
    command -v udhcpc >/dev/null 2>&1 || { log "dhcp: udhcpc not available"; return 1; }
    IMIRA_DHCP_OUT="$out" udhcpc -i "$1" -f -q -n -t 6 -T 2 -A 1 \
        -s /usr/libexec/imira/imira-dhcp.sh >/dev/null 2>&1
    [ -s "$out" ] || { log "dhcp: no lease on $1 (sink offers no DHCP?)"; return 1; }
    cat "$out"
}

ip2int() {
    local IFS=.
    set -- $1
    echo $(( ($1 << 24) | ($2 << 16) | ($3 << 8) | $4 ))
}

same_net() {
    # $1 und $2 im selben Netz mit Präfix $3?
    case "$1" in *[!0-9.]*|"") return 1 ;; esac
    local m=$(( (0xFFFFFFFF << (32 - $3)) & 0xFFFFFFFF ))
    [ $(( $(ip2int "$1") & m )) -eq $(( $(ip2int "$2") & m )) ]
}

net_first() {
    # Erste Adresse im Netz von $1/$2 (üblich für den GO: x.y.z.1).
    local m=$(( (0xFFFFFFFF << (32 - $2)) & 0xFFFFFFFF ))
    local n=$(( ($(ip2int "$1") & m) + 1 ))
    echo "$(( (n >> 24) & 255 )).$(( (n >> 16) & 255 )).$(( (n >> 8) & 255 )).$(( n & 255 ))"
}

is_sink() {
    # wfd_subelems: <id:2><laenge:4><device-information:4>… — die untersten
    # zwei Bits der Device Information nennen den Gerätetyp: 0 = Quelle,
    # 1/2 = Senke, 3 = beides.
    [ "${#1}" -ge 10 ] || return 1
    [ "${1:0:6}" = "000006" ] || return 1
    [ $(( 0x${1:6:4} & 3 )) -ne 0 ]
}

find_peer() {
    # Ersten Peer nehmen, der sich als Wi-Fi-Display-SENKE meldet. Die bloße
    # Anwesenheit von wfd_subelems reicht nicht: ein zweites Telefon mit
    # imira annonciert dieselben Infos als Quelle und wurde sonst als Ziel
    # ausgewählt (im Test genau so passiert).
    for a in $($W p2p_peers); do
        SUB=$($W p2p_peer "$a" | grep -m1 "^wfd_subelems=" | cut -d= -f2-)
        if is_sink "$SUB"; then
            echo "$a"
            return 0
        fi
    done
    return 1
}

abort_requested() {
    [ -e /tmp/imira-stop ] && return 0
    # App-Heartbeat fehlt oder ist alt: App wurde geschlossen → abbrechen.
    [ -e /tmp/imira-app-alive ] || return 0
    local now mt
    now=$(date +%s)
    mt=$(stat -c %Y /tmp/imira-app-alive 2>/dev/null || echo 0)
    [ $((now - mt)) -gt 10 ]
}

# Ein Funkteil, zwei Kanäle: Ist das Telefon mit einem WLAN verbunden und
# läuft die Miracast-Gruppe auf einem anderen Kanal, muss der Chip ständig
# hin- und herspringen. Während er beim Router ist, ist er für den Sink taub
# — Pakete gehen verloren, der Sink fordert laufend Schlüsselbilder an, das
# Bild wird schlecht oder schwarz, der Ton stockt; am schlimmsten bei großen
# Bildwechseln (App-Wechsel, Drehen). Deshalb die Gruppe auf den Kanal des
# WLANs legen, wenn Wi-Fi Direct ihn benutzen darf.
STA=$(sta_freq)
STAFREQ=""
if [ -z "$FREQ" ] && [ -n "$STA" ]; then
    if p2p_freq_ok "$STA"; then
        STAFREQ=$STA
        log "phone Wi-Fi on $STA MHz: asking the receiver for the same channel (one radio, no channel hopping)"
    else
        log "phone Wi-Fi on $STA MHz, a radar (DFS) channel Wi-Fi Direct may not use:" \
            "the radio will hop between two channels, which costs packets." \
            "If the picture breaks up: disconnect from Wi-Fi while casting, or move the router to another channel."
    fi
fi

for v in $(seq 1 "$MAX"); do
    abort_requested && exit 1
    log "attempt $v/$MAX: searching${IMIRA_PEER:+ for $IMIRA_PEER}"
    # Marke im Supplicant-Log: ab hier zählen die Ereignisse dieses Versuchs.
    MARK=$(wc -l < "$WPALOG" 2>/dev/null || echo 0)
    $W p2p_find >/dev/null
    # Auf geteiltem Radio (interner Chip: wlan0+p2p0) dauert Discovery —
    # warten, bis der Ziel-Peer wirklich sichtbar ist, sonst scheitert
    # prov_disc/connect sofort mit FAIL.
    PEER=""
    for warte in 1 2 3 4 5 6 7 8 9 10 11 12; do
        sleep 2
        abort_requested && exit 1
        if [ -n "${IMIRA_PEER:-}" ]; then
            $W p2p_peers | grep -qi "$IMIRA_PEER" && { PEER="$IMIRA_PEER"; break; }
        else
            PEER=$(find_peer || true)
            [ -n "$PEER" ] && break
        fi
    done
    if [ -z "$PEER" ]; then
        # Was stattdessen sichtbar war — zeigt z.B., ob der Sink da ist,
        # sich aber nicht als WFD-Senke meldet (kein/anderer wfd_subelems).
        log "attempt $v: no Miracast sink found after $((warte * 2)) s (receiver on and in Miracast mode?)"
        for a in $($W p2p_peers); do
            log "    visible: $a $(peer_info "$a")"
        done
        log_events
        continue
    fi
    log "attempt $v: sink $PEER found after $((warte * 2)) s"
    log "    peer: $(peer_info "$PEER")"
    # Ziel für die App sichtbar machen ("Verbinde … mit wem?").
    NAME=$($W p2p_peer "$PEER" 2>/dev/null | grep -m1 "^device_name=" | cut -d= -f2-)
    echo "${NAME:-$PEER}" > /tmp/imira-target
    chmod 644 /tmp/imira-target 2>/dev/null
    R=$($W p2p_prov_disc "$PEER" pbc 2>&1)
    log "prov_disc $PEER pbc: $R"
    sleep 2
    FREQARG=""
    [ -n "$FREQ" ] && FREQARG="freq=$FREQ"
    # Die ersten zwei Versuche mit dem WLAN-Kanal; lehnt der Sink ihn ab
    # (GO-NEG-FAILURE), geht es ohne Vorgabe weiter.
    [ -z "$FREQARG" ] && [ -n "$STAFREQ" ] && [ "$v" -le 2 ] && FREQARG="freq=$STAFREQ"
    R=$($W p2p_connect "$PEER" pbc go_intent=0 $FREQARG 2>&1)
    log "connect $PEER pbc go_intent=0 ${FREQARG:-freq=auto}: $R"
    if [ "$R" != "OK" ]; then
        # Ein nacktes FAIL kommt vom eigenen Supplicant, bevor ein Frame
        # rausgeht (Peer unbekannt/nur als Gruppen-Client gesehen, kein
        # Kanal, Gruppen-Interface nicht anlegbar). Den Peer-Zustand genau
        # jetzt festhalten; die Begründung steht nur im Debug-Log.
        log "    peer now: $(peer_info "$PEER")"
        log "    p2p status: $($W status 2>/dev/null | grep -E '^(p2p_state|wpa_state|p2p_device_address)=' | tr '\n' ' ')"
    fi
    for w in $(seq 1 12); do
        abort_requested && { $W p2p_group_remove '*' >/dev/null 2>&1; exit 1; }
        sleep 2
        # Ereignisse aus dem Supplicant-Log lesen, NICHT aus dem Journal:
        # journald ist auf manchen Geräten winzig und ratenbegrenzt (J2:
        # volatile, 1 MB, Burst 300) und verschluckt genau diese Zeilen.
        J=$(sed -n "$((MARK + 1)),\$p" "$WPALOG" 2>/dev/null)
        # Das Gruppen-Interface kann vom Basis-Interface abweichen (der
        # interne Treiber legt z.B. virtuelle p2p-Interfaces an) — deshalb
        # den Namen aus dem Ereignis übernehmen und mit ausgeben.
        LINE=$(echo "$J" | grep -E "P2P-GROUP-STARTED [^ ]+ client" | tail -1)
        if [ -n "$LINE" ]; then
            GIF=$(echo "$LINE" | sed -E "s/.*P2P-GROUP-STARTED ([^ ]+) client.*/\1/")
            GIF=${GIF:-$IFACE}
            log "attempt $v: group up after $((w * 2)) s: ${LINE#*P2P-GROUP-STARTED }"
            GF=$(echo "$LINE" | grep -oE " freq=[0-9]+" | cut -d= -f2)
            NOW_STA=$(sta_freq)
            # Für die App: "<art> <wlan-mhz> <cast-mhz>" — art = alone |
            # shared | dfs (WLAN auf Radar-Kanal, nicht teilbar) | hop (der
            # Sink wollte den gemeinsamen Kanal nicht). Die App warnt bei
            # dfs/hop, dass Aussetzer daher kommen können.
            if [ -z "$NOW_STA" ]; then
                log "radio: phone Wi-Fi not connected, cast alone on ${GF:-?} MHz"
                RADIO="alone 0 ${GF:-0}"
            elif [ "$NOW_STA" = "$GF" ]; then
                log "radio: cast and phone Wi-Fi share $GF MHz (good)"
                RADIO="shared $NOW_STA ${GF:-0}"
            else
                log "radio: cast on ${GF:-?} MHz, phone Wi-Fi on $NOW_STA MHz — DIFFERENT channels, the radio has to hop"
                if p2p_freq_ok "$NOW_STA"; then
                    RADIO="hop $NOW_STA ${GF:-0}"
                else
                    RADIO="dfs $NOW_STA ${GF:-0}"
                fi
            fi
            echo "$RADIO" > /tmp/imira-radio
            chmod 644 /tmp/imira-radio 2>/dev/null
            MY=$(echo "$LINE" | grep -oE " ip_addr=[0-9.]+" | cut -d= -f2)
            GO=$(echo "$LINE" | grep -oE "go_ip_addr=[0-9.]+" | cut -d= -f2)
            PFX=24
            SRC=eapol
            # Testschalter: EAPOL-Adresse ignorieren, DHCP erzwingen.
            [ -e /etc/imira/force-dhcp ] && { MY=""; GO=""; log "force-dhcp set, ignoring EAPOL address"; }
            if [ -z "$MY" ]; then
                SRC=dhcp
                L=$(dhcp_lease "$GIF")
                if [ -n "$L" ]; then
                    set -- $L
                    MY=$1; PFX=$4
                    [ "$MY" = "-" ] && MY=""
                    log "dhcp: lease $L"
                    # Sink = Router, sonst DHCP-Server — aber nur, wenn er im
                    # Lease-Netz liegt. Der hichip-Projektor meldet als
                    # Server eine öffentliche Adresse; der Sink sitzt dann
                    # (wie bei Android-GOs üblich) auf der .1 des Netzes.
                    GO=""
                    for c in "$2" "$3"; do
                        [ "$c" != "-" ] && [ -n "$MY" ] && same_net "$c" "$MY" "$PFX" && { GO=$c; break; }
                    done
                    if [ -z "$GO" ] && [ -n "$MY" ]; then
                        GO=$(net_first "$MY" "$PFX")
                        log "dhcp: router/server outside $MY/$PFX, assuming sink at $GO"
                    fi
                else
                    SRC=guess
                fi
            fi
            if [ -z "$MY" ] || [ -z "$GO" ]; then
                SRC=guess
                MY=${MY:-192.168.157.100}; GO=${GO:-192.168.157.1}
            fi
            log "address: $MY/$PFX, sink $GO (source: $SRC)"
            log_events
            echo "$GIF $MY $GO $PFX"
            exit 0
        fi
        echo "$J" | grep -qE "FORMATION-FAILURE|GO-NEG-FAILURE" && break
    done
    log "attempt $v failed"
    log_events
    $W p2p_group_remove '*' >/dev/null 2>&1
    $W p2p_stop_find >/dev/null 2>&1
    sleep 3
done
exit 1
