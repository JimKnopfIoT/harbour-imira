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
import bisect
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
    return m.group(1) + "[other device]" + m.group(3)


def _ssid_value(v):
    if v.startswith("DIRECT-"):
        # DIRECT-xy-<Name>: Gruppenkennung bleibt, fremde Namen nicht.
        rest = v[9:]
        if rest and not (TARGET and rest.strip("- ") in TARGET):
            return v[:9] + "-[name]"
        return v
    return "[ssid]"


def _ssid_q(m):
    return m.group(1) + m.group(2) + _ssid_value(m.group(3)) + m.group(2)


def _ip4(m):
    a = [int(x) for x in m.group(0).split(".")]
    if any(x > 255 for x in a):
        return m.group(0)
    private = (a[0] in (0, 10, 127, 255) or (a[0] == 172 and 16 <= a[1] <= 31)
               or (a[0] == 192 and a[1] == 168) or (a[0] == 169 and a[1] == 254)
               or a[0] >= 224)
    return m.group(0) if private else "[ip]"


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
        text = text.replace(n, "[other device]")
    return text


def anon(text, ips=True):
    text = MAC_US_RE.sub(lambda m: _mac(re.match(r".*", m.group(0).replace("_", ":"))).replace(":", "_"), text)
    text = MAC_RE.sub(_mac, text)
    text = UUID_RE.sub("[uuid]", text)
    text = SERIAL_RE.sub(lambda m: m.group(1) + "[serial]", text)
    for r in NAME_RES:
        text = r.sub(_name, text)
    text = SSID_Q_RE.sub(_ssid_q, text)
    text = SSID_IW_RE.sub(lambda m: m.group(1) + _ssid_value(m.group(2).strip()), text)
    text = HOME_RE.sub("/home/[user]", text)
    if ips:
        text = IP4_RE.sub(_ip4, text)
        # Nur echte IPv6 (mit "::" oder acht Gruppen); Uhrzeiten wie
        # 14:29:05 haben weder noch.
        text = IP6_RE.sub(lambda m: "[ipv6]" if ("::" in m.group(0) or m.group(0).count(":") == 7) else m.group(0), text)
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


# Jede Suchrunde (alle ~0,3 s) wiederholt dieselben Attribut- und Funk-
# Zeilen. Sie füllten den ausführlichen Log so, dass beim Kürzen die
# Gruppenbildung verloren ging.
WPA_NOISE = re.compile(
    r"WPS:  \* |P2P: \* |P2P: Parsing |P2P: Attribute |Config Methods \(WPS\)|"
    r"Primary Device Type \(WPS\)|Device Capability .* Group Capability|"
    r"P2P: Device Info|radio work|Radio work|radio_work|emain-on-channel|"
    r"Off-channel: Send Action callback|Listen timeout|Listen state|"
    r"P2P: Timeout \(state=SEARCH\)|P2P: Starting search|P2P: Set timeout|"
    r"P2P: State SEARCH -> SEARCH|stop_listen|DPP: Remain|SCAN_RESULTS|"
    r"SCAN_STARTED|NL80211_CMD_(NEW_SCAN_RESULTS|TRIGGER_SCAN)|"
    r"Do not update peer entry|P2P probe - mask|short listen state|REMAIN_ON_CHANNEL|"
    r"Add operating class|Supported (vendor|cipher)|Register frame type=|"
    r"BSS: No P2P IE|channels:|Channels - hexdump|DEL_KEY|broadcast key|"
    r"Control interface recv command from")
# Was eine Verbindung ausmacht: Um diese Zeilen herum bleibt alles stehen.
WPA_FOCUS = re.compile(
    r"P2P_CONNECT|P2P_PROV_DISC|P2P-PROV-DISC|P2P-GO-NEG|GO Negotiation|"
    r"P2P-GROUP-|P2P-INVITATION|AP-STA-|AP-ENABLED|WPS-[A-Z]|CTRL-EVENT-EAP|"
    r"CTRL-EVENT-(CONNECTED|DISCONNECTED)|Register frame command|"
    r"IEEE 802\.11: (auth|assoc)")


def wpa_is_cast(path):
    """Gehörte dieser Supplicant-Lauf zu einem Cast (nicht zum Diagnose-Scan)?"""
    try:
        with open(path, errors="replace") as f:
            return any("P2P_CONNECT" in l or "GO-NEG" in l or "PROV-DISC" in l for l in f)
    except OSError:
        return False


def filter_wpa(path, limit):
    try:
        with open(path, errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return "(missing)"
    if not lines:
        return "(empty)"
    total = len(lines)
    lines = [l for l in lines if not WPA_NOISE.search(l)]
    # Zeitfenster von je 2 s um jedes Verbindungsereignis: Dort zählt jede
    # Zeile, draußen nur der grobe Filter. Ein Fenster vom ersten bis zum
    # letzten Ereignis umfasste die ganze Sitzung, und beim Kürzen blieben
    # nur Start und Abbau des Supplicants übrig.
    def ts(l):
        try:
            return float(l.split(":", 1)[0])
        except ValueError:
            return None
    marks = sorted(t for t in (ts(l) for l in lines if WPA_FOCUS.search(l)) if t is not None)
    def near(t):
        i = bisect.bisect_left(marks, t - 2)
        return i < len(marks) and marks[i] <= t + 2
    keep = []
    for l in lines:
        t = ts(l)
        if t is not None and near(t) and not re.search(r"hexdump|RX mgmt|Event message available", l):
            keep.append(l)
        elif WPA_KEEP.search(l) and not WPA_DROP.search(l) \
                or re.search(r"P2P-GROUP-STARTED|P2P-GROUP-REMOVED|GO-NEG|FORMATION", l):
            keep.append(l)
    note = "(%d of %d lines kept)" % (len(keep), total)
    if len(keep) > limit:
        keep = ["(... %d earlier kept lines omitted)" % (len(keep) - limit)] + keep[-limit:]
    return note + "\n" + "\n".join(keep)


def cut_focus(body, size):
    """Wie cut(), aber von den Verbindungsereignissen nach außen: Behalten
    wird jede Zeile, die höchstens r Zeilen von einem Ereignis entfernt
    ist, r so groß, wie es passt. Lücken bekommen einen Vermerk."""
    lines = body.splitlines()
    if len(body) <= size or not lines:
        return body
    focus = [i for i, l in enumerate(lines) if WPA_FOCUS.search(l)]
    if not focus:
        return cut(body, size, "tail")
    dist = [len(lines)] * len(lines)
    for i in focus:
        dist[i] = 0
    for i in range(1, len(lines)):
        dist[i] = min(dist[i], dist[i - 1] + 1)
    for i in range(len(lines) - 2, -1, -1):
        dist[i] = min(dist[i], dist[i + 1] + 1)
    def build(r):
        out, gap = [], 0
        for i, l in enumerate(lines):
            if dist[i] <= r or i == 0:
                if gap:
                    out.append("(... %d lines cut)" % gap)
                    gap = 0
                out.append(l)
            else:
                gap += 1
        if gap:
            out.append("(... %d lines cut)" % gap)
        return "\n".join(out)
    lo, hi = -1, len(lines)
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if len(build(mid)) <= size:
            lo = mid
        else:
            hi = mid
    if lo < 0:
        # Nicht einmal die Ereignisse allein passen: die letzten behalten.
        return cut("\n".join(lines[i] for i in focus), size, "tail")
    return build(lo)


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


# Das Sailfish-Forum nimmt höchstens 50 000 Zeichen pro Beitrag — mit
# Luft für ``` und ein paar Worte des Nutzers.
REPORT_BUDGET = 45000


def cut(body, size, keep):
    """body auf etwa size Zeichen kürzen, ganze Zeilen, mit Vermerk."""
    lines = body.splitlines()
    if len(body) <= size or not lines:
        return body
    size = max(size - 60, 0)
    if keep == "head":
        head_n, tail_n = size, 0
    elif keep == "headtail":
        head_n, tail_n = size // 2, size - size // 2
    else:
        head_n, tail_n = 0, size
    head, used = [], 0
    for l in lines:
        if used + len(l) + 1 > head_n:
            break
        head.append(l)
        used += len(l) + 1
    tail, used = [], 0
    for l in reversed(lines[len(head):]):
        if used + len(l) + 1 > tail_n:
            break
        tail.insert(0, l)
        used += len(l) + 1
    gone = len(lines) - len(head) - len(tail)
    return "\n".join(head + ["(... %d lines cut to fit a forum post)" % gone] + tail)


def fit(parts, budget):
    """Kürzbare Abschnitte (Gewicht > 0) so beschneiden, dass alles zusammen
    ins Budget passt. Wasserstand: jeder bekommt höchstens Gewicht × Pegel,
    der Pegel so hoch wie möglich; kleine Abschnitte bleiben ganz."""
    fixed = sum(len(t) + len(b) + 1 for t, b, w, _k in parts if w == 0) \
        + sum(len(t) + 1 for t, b, w, _k in parts if w > 0)
    free = budget - fixed
    cand = [p for p in parts if p[2] > 0]
    if sum(len(p[1]) for p in cand) <= free:
        return
    lo, hi = 0.0, float(max(len(p[1]) for p in cand))
    for _ in range(40):
        mid = (lo + hi) / 2
        if sum(min(len(p[1]), p[2] * mid) for p in cand) <= free:
            lo = mid
        else:
            hi = mid
    for p in cand:
        size = int(p[2] * lo)
        p[1] = cut_focus(p[1], size) if p[3] == "focus" else cut(p[1], size, p[3])


def main():
    now = datetime.datetime.now(datetime.timezone.utc)
    rel = os_release()
    status = first_line("/tmp/imira-status")
    iface = status.split(" ")[3] if len(status.split(" ")) > 3 else "-"
    if iface == "-":
        iface = "p2p0"
    flags = sorted(os.path.basename(p) for p in glob.glob("/etc/imira/*") if not p.endswith(".conf"))

    parts = []

    def sec(title, body, ips=True, weight=0, keep="tail"):
        # weight > 0: darf gekürzt werden, damit der Bericht in einen
        # Forenbeitrag passt (siehe fit()); größeres Gewicht = mehr Platz.
        parts.append(["\n===== %s =====\n" % title, anon(body, ips), weight, keep])

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
        "audio codec: setting %s, last cast %s, AAC encoder %s" % (
            first_line("/tmp/imira-audio-codec-pref", "auto"),
            first_line("/tmp/imira-audio-codec", "-"),
            first_line("/tmp/imira-aac-probe", "not probed")),
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
        "removed. Please look it over before posting it. In the forum, paste",
        "it unchanged between two lines of ``` (keeps the line breaks).",
    ]
    # Kopf ohne IP-Filter: Versionsnummern wie 5.2.0.17 sähen sonst wie
    # IPv4-Adressen aus.
    parts.append([anon("\n".join(head), ips=False) + "\n", "", 0, "tail"])

    # Reihenfolge nach Nutzen: Was eine Sitzung erklärt (RTSP, Ton) steht
    # vorn, die langen Funk-Logs hinten — wird ein Beitrag doch abgeschnitten,
    # fehlt das Unwichtigste.
    sec("diagnostic scan (receivers nearby)", read("/tmp/imira-diag-scan.log", 200), weight=1)
    sec("RTSP handshake, current attempt", read_proto("/tmp/imira-proto.log", 400), weight=2,
        keep="headtail")
    sec("audio log (outputs, streams, route switches, level every 5 s)",
        read_head_tail("/tmp/imira-audio.log", 60, 240), weight=2, keep="headtail")
    sec("RTSP handshake, earlier attempts", read_proto("/tmp/imira-proto.prev.log", 400), weight=2)
    sec("connect log", read("/tmp/imira-connect.log", 500), weight=3)
    if first_line("/tmp/imira-mode", "") == "convergence":
        sec("convergence compositor", read("/tmp/imira-comp.log", 80), weight=1)
    # Alle WLAN-Interfaces mit Typ, auch fremde Namen: Der J2-Treiber erlaubt
    # nur zwei zusätzliche, belegte Plätze lassen das connect sofort scheitern.
    sec("wireless interfaces", sh("iw dev 2>/dev/null | grep -E '^phy|Interface|type'"))
    sec("interfaces", sh("ip -o link 2>/dev/null | awk '{print $2, $3, $9}' | grep -E '^(wlan|p2p)'")
        + "\n" + sh("ip -4 -o addr 2>/dev/null | awk '{print $2, $4}' | grep -E '^(wlan|p2p)'"))
    # Ob der Sink unseren RTSP-Port 7236 überhaupt erreichen darf.
    sec("firewall (INPUT)", sh("iptables -S INPUT 2>/dev/null | head -20; "
                               "iptables -S connman-INPUT 2>/dev/null | head -60"), weight=1)
    if SURVEY:
        sec("radio environment", survey(), weight=1)
    sec("wireless capabilities", iw_phy_summary(), weight=1, keep="head")
    # Ein Diagnose-Scan nach dem Cast ist selbst ein Supplicant-Lauf und
    # schöbe den Cast nach hinten: Cast-Läufe bekommen den Platz, Scans
    # wenig. Gekürzt wird von den Verbindungsereignissen nach außen.
    for i, p in enumerate(("/tmp/imira-wpa.log", "/tmp/imira-wpa.log.1", "/tmp/imira-wpa.log.2")):
        cast = wpa_is_cast(p)
        sec("wpa_supplicant log (%s, %s)" % ("latest run" if i == 0 else "%d run(s) earlier" % i,
                                             "cast" if cast else "scan only"),
            filter_wpa(p, 6000), weight=(4 if cast else 1), keep="focus" if cast else "tail")

    fit(parts, REPORT_BUDGET)
    parts = [t + b + ("\n" if b else "") for t, b, _w, _k in parts]

    text = forget_foreign("".join(parts))

    pw = pwd.getpwnam(USER)
    docs = os.path.join(pw.pw_dir, "Documents")
    os.makedirs(docs, exist_ok=True)
    path = os.path.join(docs, "imira-report-%s.txt" % now.astimezone().strftime("%Y%m%d-%H%M%S"))
    with open(path, "w") as f:
        f.write(text)
    os.chown(path, pw.pw_uid, pw.pw_gid)
    os.chmod(path, 0o644)
    print(path)


if __name__ == "__main__":
    main()
