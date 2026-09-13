#!/bin/bash
# imira-connect.sh — Wi-Fi-Direct-Verbindung zu einem WFD-Sink aufbauen.
# Der Sink muss Group Owner werden (Rollen-Verhandlung ist ein Münzwurf,
# daher Retry-Schleife). Erfolgreich, wenn P2P-GROUP-STARTED ... client.
#
# Env: IMIRA_IFACE (wlan1|p2p0), IMIRA_CTRL (ctrl-Socket-Dir),
#      IMIRA_WPA_LOG (Logdatei des Supplicants), IMIRA_PEER (optional MAC),
#      IMIRA_FREQ (default 2437), IMIRA_ATTEMPTS (default 12)
# Ausgabe bei Erfolg auf stdout: "<eigene-ip> <go-ip>" (aus dem Group-Event).
set -u
IFACE="${IMIRA_IFACE:-wlan1}"
CTRL="${IMIRA_CTRL:-/var/run/wpa_imira}"
WPALOG="${IMIRA_WPA_LOG:-/tmp/imira-wpa.log}"
FREQ="${IMIRA_FREQ:-}"   # leer = Treiber/GO wählt den Kanal
MAX="${IMIRA_ATTEMPTS:-12}"
W="/usr/libexec/imira/wpa_cli-p2p -p $CTRL -i $IFACE"

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

for v in $(seq 1 "$MAX"); do
    abort_requested && exit 1
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
        echo "versuch $v: Ziel nicht sichtbar (Sink eingeschaltet/bereit?)" >&2
        continue
    fi
    # Ziel für die App sichtbar machen ("Verbinde … mit wem?").
    NAME=$($W p2p_peer "$PEER" 2>/dev/null | grep -m1 "^device_name=" | cut -d= -f2-)
    echo "${NAME:-$PEER}" > /tmp/imira-target
    chmod 644 /tmp/imira-target 2>/dev/null
    R=$($W p2p_prov_disc "$PEER" pbc 2>&1)
    echo "prov_disc $PEER: $R" >&2
    sleep 2
    FREQARG=""
    [ -n "$FREQ" ] && FREQARG="freq=$FREQ"
    R=$($W p2p_connect "$PEER" pbc go_intent=0 $FREQARG 2>&1)
    echo "connect $PEER: $R" >&2
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
            MY=$(echo "$LINE" | grep -oE "ip_addr=[0-9.]+" | cut -d= -f2)
            GO=$(echo "$LINE" | grep -oE "go_ip_addr=[0-9.]+" | cut -d= -f2)
            echo "${GIF:-$IFACE} ${MY:-192.168.157.100} ${GO:-192.168.157.1}"
            exit 0
        fi
        echo "$J" | grep -qE "FORMATION-FAILURE|GO-NEG-FAILURE" && break
    done
    echo "versuch $v fehlgeschlagen" >&2
    $W p2p_group_remove '*' >/dev/null 2>&1
    $W p2p_stop_find >/dev/null 2>&1
    sleep 3
done
exit 1
