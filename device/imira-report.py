#!/usr/bin/env python3
# imira-report.py — Diagnosebericht für Fremd-Geräte (von imira-session.sh
# als root aufgerufen, ausgelöst über die Diagnose-Seite der App).
#
# Sammelt System-/WLAN-Daten, den Diagnose-Scan, connect/proto/wpa-Logs und
# optional die Funkumgebung, anonymisiert alles und legt es als Textdatei in
# ~/Documents ab. Gibt den Pfad auf stdout aus. Inhalt englisch: der Bericht
# geht an Leute aus aller Welt.
#
# Anonymisierung (bewusst Positivliste, lieber eine Zeile zu wenig als ein
# Heimnetzname zu viel):
#   - MAC-Adressen: Herstellerteil bleibt, Rest wird pro Adresse durchnummeriert
#   - Gerätenamen fremder Peers, SSIDs (außer DIRECT-xy der Gruppe), UUIDs,
#     Seriennummern, öffentliche IPv4- und alle IPv6-Adressen fliegen raus
#   - aus dem ausführlichen wpa-Log nur P2P/WPS/WFD/Verbindungszeilen, keine
#     Hexdumps (darin stehen Namen und SSIDs roh)
import datetime
import glob
import os
import pwd
import re
import subprocess

USER = "defaultuser"
OPTS = os.environ.get("IMIRA_REPORT_OPTS", "")
SURVEY = "survey" in OPTS


def sh(cmd, limit=None):
    try:
        out = subprocess.run(cmd, shell=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=20).stdout
        out = out.decode(errors="replace")
    except Exception as e:  # noqa: BLE001 — ein Bericht darf nie abbrechen
        out = "(failed: %s)\n" % e
    lines = out.splitlines()
    if limit and len(lines) > limit:
        lines = ["(... %d earlier lines omitted)" % (len(lines) - limit)] + lines[-limit:]
    return "\n".join(lines)


def read(path, limit=None):
    try:
        with open(path, errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return "(missing)"
    if limit and len(lines) > limit:
        lines = ["(... %d earlier lines omitted)" % (len(lines) - limit)] + lines[-limit:]
    return "\n".join(lines) if lines else "(empty)"


def read_head_tail(path, head, tail):
    # Anfang (Ausgänge, erste Ströme) und Ende (letzte Umschaltungen) —
    # die Pegelzeilen dazwischen sind entbehrlich.
    try:
        with open(path, errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return "(missing)"
    if len(lines) > head + tail:
        lines = lines[:head] + ["(... %d lines omitted)" % (len(lines) - head - tail)] + lines[-tail:]
    return "\n".join(lines) if lines else "(empty)"


def first_line(path, default="-"):
    try:
        with open(path, errors="replace") as f:
            return f.readline().strip() or default
    except OSError:
        return default


# ---------------------------------------------------------------- anonymizer
TARGET = first_line("/tmp/imira-target", "")
OWN_NAMES = {"Sailfish", ""}
_macs = {}


def _mac(m):
    full = m.group(0).lower().replace("-", ":")
    if full in ("00:00:00:00:00:00", "ff:ff:ff:ff:ff:ff"):
        return full
    if full not in _macs:
        _macs[full] = "%s:xx:xx:%02d" % (full[:8], len(_macs) + 1)
    return _macs[full]


def _keep_name(name):
    n = name.strip()
    return n in OWN_NAMES or (TARGET and n == TARGET)


_foreign = set()


def _name(m):
    if _keep_name(m.group(2)):
        return m.group(0)
    if len(m.group(2).strip()) >= 3:
        _foreign.add(m.group(2).strip())
    return m.group(1) + "<other device>" + m.group(3)


def _ssid_value(v):
    if v.startswith("DIRECT-"):
        # DIRECT-xy-<Name>: Gruppenkennung bleibt, fremde Namen nicht.
        rest = v[9:]
        if rest and not (TARGET and rest.strip("- ") in TARGET):
            return v[:9] + "-<name>"
        return v
    return "<ssid>"


def _ssid_q(m):
    return m.group(1) + m.group(2) + _ssid_value(m.group(3)) + m.group(2)


def _ip4(m):
    a = [int(x) for x in m.group(0).split(".")]
    if any(x > 255 for x in a):
        return m.group(0)
    private = (a[0] in (0, 10, 127, 255) or (a[0] == 172 and 16 <= a[1] <= 31)
               or (a[0] == 192 and a[1] == 168) or (a[0] == 169 and a[1] == 254)
               or a[0] >= 224)
    return m.group(0) if private else "<ip>"


# Grenzen: kein Hex davor/danach, auch nicht über einen Doppelpunkt hinweg —
# aber ein Doppelpunkt als Satzzeichen ("connect aa:…:ff: OK") ist erlaubt.
MAC_RE = re.compile(r"(?<![0-9a-fA-F])(?<![0-9a-fA-F][:-])(?:[0-9a-fA-F]{2}[:-]){5}[0-9a-fA-F]{2}(?![:-]?[0-9a-fA-F])")
MAC_ANY_RE = re.compile(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}")
# PulseAudio schreibt Bluetooth-MACs mit Unterstrichen in Ausgangsnamen
# (bluez_sink.AA_BB_CC_DD_EE_FF.a2dp_sink) — die stehen im Ton-Log.
MAC_US_RE = re.compile(r"(?<![0-9a-fA-F])(?:[0-9a-fA-F]{2}_){5}[0-9a-fA-F]{2}(?![0-9a-fA-F])")
NAME_RES = [
    re.compile(r"(device_name=)(.*?)(\s+(?=[a-z_]+=)|$)"),
    re.compile(r"(\bname=')(.*?)(')"),
    re.compile(r"(?i)(device[ _]name[ =:]*')(.*?)(')"),
    re.compile(r"(dev_name=)(.*?)(\s+(?=[a-z_]+=)|$)"),
]
SSID_Q_RE = re.compile(r"((?i:ssid)\s*[=:]?\s*)([\"'])(.*?)\2")
SSID_IW_RE = re.compile(r"^(\s*ssid )(.+)$", re.M)
UUID_RE = re.compile(r"(?i)\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b")
SERIAL_RE = re.compile(r"(?i)(serial[ _]?number\s*[=:']\s*)\S+")
IP4_RE = re.compile(r"(?<![\d.])\d{1,3}(?:\.\d{1,3}){3}(?![\d.])")
IP6_RE = re.compile(r"(?i)(?<![0-9a-fx:])(?:[0-9a-f]{0,4}:){2,7}[0-9a-f]{0,4}(?![0-9a-fx:])")
HOME_RE = re.compile(r"/home/(?!defaultuser\b)[^/\s]+")


def forget_foreign(text):
    # Zweites Netz: jeder einmal erkannte fremde Name wird überall ersetzt,
    # in welchem Format er auch sonst noch auftaucht (SSID, Debugzeile, …).
    for n in sorted(_foreign, key=len, reverse=True):
        text = text.replace(n, "<other device>")
    return text


def anon(text, ips=True):
    text = MAC_US_RE.sub(lambda m: _mac(re.match(r".*", m.group(0).replace("_", ":"))).replace(":", "_"), text)
    text = MAC_RE.sub(_mac, text)
    text = UUID_RE.sub("<uuid>", text)
    text = SERIAL_RE.sub(lambda m: m.group(1) + "<serial>", text)
    for r in NAME_RES:
        text = r.sub(_name, text)
    text = SSID_Q_RE.sub(_ssid_q, text)
    text = SSID_IW_RE.sub(lambda m: m.group(1) + _ssid_value(m.group(2).strip()), text)
    text = HOME_RE.sub("/home/<user>", text)
    if ips:
        text = IP4_RE.sub(_ip4, text)
        # Nur echte IPv6 (mit "::" oder acht Gruppen); Uhrzeiten wie
        # 14:29:05 haben weder noch.
        text = IP6_RE.sub(lambda m: "<ipv6>" if ("::" in m.group(0) or m.group(0).count(":") == 7) else m.group(0), text)
    # Sicherheitsnetz: was an MAC-artigem noch übrig ist, ohne Grenzprüfung.
    text = MAC_ANY_RE.sub(_mac, text)
    return text


# Ausführliches wpa-Log: nur was für P2P/WFD/Verbindungsaufbau zählt.
WPA_KEEP = re.compile(
    r"P2P|p2p|WFD|wfd|WPS|EAPOL|GO Neg|Invitation|Provision|GROUP|Group|"
    r"CTRL-EVENT-(CONNECTED|DISCONNECTED|ASSOC-REJECT|AUTH-REJECT)|"
    r"nl80211: (Connect|Associat|Authenticat|Remote-on-channel|Send Action|"
    r"Frame TX|Drv Event|Set mode|Create interface|Remove interface|"
    r"interface .* in phy)|"
    r"State: |Failed|failed|FAIL|rejected|refused|error|Error|"
    r"freq|channel|Channel")
WPA_DROP = re.compile(
    r"hexdump|BSS: |Scan|scan|wpa_scan|SSID|ssid|Probe Request|RX mgmt|"
    r"P2P-DEVICE-FOUND|P2P-DEVICE-LOST|nl80211: Event message available")


def filter_wpa(path, limit):
    try:
        with open(path, errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return "(missing)"
    if not lines:
        return "(empty)"
    keep = [l for l in lines if WPA_KEEP.search(l) and not WPA_DROP.search(l)
            or re.search(r"P2P-GROUP-STARTED|P2P-GROUP-REMOVED|GO-NEG|FORMATION", l)]
    note = "(%d of %d lines kept)" % (len(keep), len(lines))
    if len(keep) > limit:
        keep = ["(... %d earlier kept lines omitted)" % (len(keep) - limit)] + keep[-limit:]
    return note + "\n" + "\n".join(keep)


KEEPALIVE_TX = re.compile(r">>> GET_PARAMETER rtsp://localhost/wfd1\.0 RTSP/1\.0 \| CSeq: \d+ \| Session: \S+ \|  \| $")
EMPTY_OK_RX = re.compile(r"<<< RTSP/1\.0 200 OK \| CSeq: \d+ \|BODY\| $")


def read_proto(path, limit):
    # Keepalives (alle 15 s) und castds Access-Unit-Zeilen würden in langen
    # Sitzungen den Handshake aus dem Zeilenlimit drängen: zählen statt zeigen.
    try:
        with open(path, errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return "(missing)"
    out, ka, au = [], 0, 0
    pending_ka = False
    for l in lines:
        if KEEPALIVE_TX.search(l):
            ka += 1
            pending_ka = True
            continue
        if pending_ka and EMPTY_OK_RX.search(l):
            pending_ka = False
            continue
        pending_ka = False
        if "imira-castd: AU size=" in l:
            au += 1
            if au > 5:
                continue
        out.append(l)
    if ka or au > 5:
        out.append("(%d keepalive exchanges and %d further access-unit lines not shown)"
                   % (ka, max(0, au - 5)))
    if len(out) > limit:
        out = out[:limit // 2] + ["(... %d lines omitted)" % (len(out) - limit)] + out[-limit // 2:]
    return "\n".join(out) if out else "(empty)"


# ---------------------------------------------------------------- sections
def os_release():
    d = {}
    for p in ("/etc/os-release", "/etc/hw-release"):
        try:
            for l in open(p, errors="replace"):
                if "=" in l:
                    k, v = l.strip().split("=", 1)
                    d[p[5:7] + k] = v.strip('"')
        except OSError:
            pass
    return d


def driver(iface):
    try:
        return os.path.basename(os.readlink("/sys/class/net/%s/device/driver" % iface))
    except OSError:
        return "-"


def iw_phy_summary():
    out = sh("iw list 2>/dev/null")
    if not out.strip():
        return "(iw not available)"
    keep, grab = [], False
    for l in out.splitlines():
        s = l.strip()
        if s.startswith("Supported interface modes") or s.startswith("valid interface combinations") \
                or s.startswith("Frequencies:"):
            grab = True
            keep.append(l)
            continue
        if grab:
            if l.startswith("\t\t\t") or l.startswith("\t\t *") or s.startswith("*") or s.startswith("#"):
                keep.append(l)
                continue
            grab = False
    return "\n".join(keep)


def survey():
    out = []
    link = sh("iw dev wlan0 link 2>/dev/null")
    m = re.search(r"freq:\s*(\d+)", link)
    out.append("phone Wi-Fi: " + ("connected, %s MHz" % m.group(1) if m else "not connected"))
    for key in ("signal", "tx bitrate"):
        mm = re.search(r"%s:\s*(.+)" % key, link)
        if mm:
            out.append("  %s: %s" % (key, mm.group(1).strip()))
    out.append("regulatory domain:")
    out.append(sh("iw reg get 2>/dev/null | grep -E 'country|phy#' | head -6"))
    # Belegung pro Frequenz aus dem Scan-Cache (kein neuer Scan, die WLAN-
    # Verbindung bleibt ungestört). Nur Anzahl und stärkstes Signal, keine
    # Namen oder Adressen.
    dump = sh("iw dev wlan0 scan dump 2>/dev/null")
    per = {}
    cur = None
    for l in dump.splitlines():
        m = re.match(r"\s*freq:\s*([\d.]+)", l)
        if m:
            cur = int(float(m.group(1)))
            per.setdefault(cur, [0, -200.0])
            per[cur][0] += 1
            continue
        m = re.match(r"\s*signal:\s*(-?[\d.]+)", l)
        if m and cur is not None:
            per[cur][1] = max(per[cur][1], float(m.group(1)))
    out.append("networks seen per frequency (from the scan cache, no names):")
    if not per:
        out.append("  (scan cache empty)")
    for f in sorted(per):
        ch = (f - 2407) // 5 if f < 2500 else (f - 5000) // 5
        out.append("  %5d MHz (ch %3d): %2d network(s), strongest %.0f dBm" % (f, ch, per[f][0], per[f][1]))
    return "\n".join(out)


def phone_wifi():
    # Nur Frequenz und Radar-Pflicht, keine Netznamen: entscheidend dafür, ob
    # das Funkteil zwischen WLAN und Miracast-Kanal hin- und herspringen muss.
    m = re.search(r"freq:\s*(\d+)", sh("iw dev wlan0 link 2>/dev/null"))
    if not m:
        return "not connected"
    f = int(m.group(1))
    dfs = 5250 < f < 5745
    return "connected on %d MHz%s" % (f, " (radar/DFS channel, Wi-Fi Direct cannot share it)" if dfs else "")


def main():
    now = datetime.datetime.now(datetime.timezone.utc)
    rel = os_release()
    status = first_line("/tmp/imira-status")
    iface = status.split(" ")[3] if len(status.split(" ")) > 3 else "-"
    if iface == "-":
        iface = "p2p0"
    flags = sorted(os.path.basename(p) for p in glob.glob("/etc/imira/*") if not p.endswith(".conf"))

    parts = []

    def sec(title, body, ips=True):
        parts.append("\n===== %s =====\n%s\n" % (title, anon(body, ips)))

    head = [
        "Imira diagnostic report",
        "created: %s UTC" % now.strftime("%Y-%m-%d %H:%M:%S"),
        "imira: %s" % sh("rpm -q --qf '%{VERSION}-%{RELEASE}' harbour-imira"),
        "Sailfish OS: %s" % rel.get("osVERSION", rel.get("osVERSION_ID", "-")),
        "device: %s" % (rel.get("hwNAME") or first_line("/sys/firmware/devicetree/base/model")).replace("\x00", ""),
        "kernel: %s" % sh("uname -r"),
        "wpa_supplicant: %s" % sh("/usr/libexec/imira/wpa_supplicant-p2p -v 2>/dev/null | head -1"),
        "cast interface: %s (driver %s), wlan0 driver %s" % (iface, driver(iface), driver("wlan0")),
        "udhcpc: %s" % ("yes" if sh("command -v udhcpc").strip() else "no"),
        "phone Wi-Fi: %s" % phone_wifi(),
        "audio route: %s (running: %s)" % (first_line("/tmp/imira-audio-route", "auto"),
                                           first_line("/tmp/imira-audio-active", "-")),
        "settings: mode %s, resolution %s, receiver %s, detailed log %s, survey %s" % (
            first_line("/tmp/imira-mode", "mirror"), first_line("/tmp/imira-res", "1080"),
            first_line("/tmp/imira-peer", "auto"),
            "on" if first_line("/var/run/imira-wpa.mode") == "debug" or os.path.exists("/tmp/imira-debug") else "off",
            "on" if SURVEY else "off"),
        "flags in /etc/imira: %s" % (", ".join(flags) or "none"),
        "last status: %s" % status,
        "",
        "Anonymized: MAC addresses cut to the vendor part, names of other",
        "devices, network names, serial numbers, UUIDs and public IP addresses",
        "removed. Please look it over before posting it.",
    ]
    # Kopf ohne IP-Filter: Versionsnummern wie 5.2.0.17 sähen sonst wie
    # IPv4-Adressen aus.
    parts.append(anon("\n".join(head), ips=False) + "\n")

    sec("diagnostic scan (receivers nearby)", read("/tmp/imira-diag-scan.log", 200))
    sec("connect log", read("/tmp/imira-connect.log", 500))
    sec("RTSP handshake, current attempt", read_proto("/tmp/imira-proto.log", 400))
    sec("RTSP handshake, earlier attempts", read_proto("/tmp/imira-proto.prev.log", 400))
    sec("audio log (outputs, streams, route switches, level every 5 s)",
        read_head_tail("/tmp/imira-audio.log", 60, 240))
    if first_line("/tmp/imira-mode", "") == "convergence":
        sec("convergence compositor", read("/tmp/imira-comp.log", 80))
    for i, p in enumerate(("/tmp/imira-wpa.log", "/tmp/imira-wpa.log.1", "/tmp/imira-wpa.log.2")):
        sec("wpa_supplicant log (%s)" % ("latest run" if i == 0 else "%d run(s) earlier" % i),
            filter_wpa(p, 3000))
    sec("wireless capabilities", iw_phy_summary())
    # Alle WLAN-Interfaces mit Typ, auch fremde Namen: Der J2-Treiber erlaubt
    # nur zwei zusätzliche, belegte Plätze lassen das connect sofort scheitern.
    sec("wireless interfaces", sh("iw dev 2>/dev/null | grep -E '^phy|Interface|type'"))
    sec("interfaces", sh("ip -o link 2>/dev/null | awk '{print $2, $3, $9}' | grep -E '^(wlan|p2p)'")
        + "\n" + sh("ip -4 -o addr 2>/dev/null | awk '{print $2, $4}' | grep -E '^(wlan|p2p)'"))
    # Ob der Sink unseren RTSP-Port 7236 überhaupt erreichen darf.
    sec("firewall (INPUT)", sh("iptables -S INPUT 2>/dev/null | head -20; "
                               "iptables -S connman-INPUT 2>/dev/null | head -60"))
    if SURVEY:
        sec("radio environment", survey())

    text = forget_foreign("".join(parts))

    pw = pwd.getpwnam(USER)
    docs = os.path.join(pw.pw_dir, "Documents")
    os.makedirs(docs, exist_ok=True)
    path = os.path.join(docs, "imira-report-%s.txt" % now.strftime("%Y%m%d-%H%M%S"))
    with open(path, "w") as f:
        f.write(text)
    os.chown(path, pw.pw_uid, pw.pw_gid)
    os.chmod(path, 0o644)
    print(path)


if __name__ == "__main__":
    main()
