#!/usr/bin/env python3
# imira-wfd-proto.py — Wi-Fi-Display-Source: führt den WFD-RTSP-Handshake
# (M1-M8) mit dem Sink und startet bei PLAY den Stream (IMIRA_STREAM_CMD,
# sonst zum Testen eine vorencodierte MPEG-TS-Datei). Log englisch.
import os
import re
import signal
import socket
import subprocess
import time

LOCAL_IP = os.environ.get("IMIRA_LOCAL_IP", "192.168.157.100")
SINK_IP = os.environ.get("IMIRA_SINK_IP", "192.168.157.1")
RTSP_PORT = int(os.environ.get("IMIRA_RTSP_PORT", "7236"))
TS_FILE = os.environ.get("IMIRA_TS_FILE", "/opt/imira/imira-test.ts")
SERVER_RTP = 19000


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


class WFDSession:
    def __init__(self, conn):
        self.c = conn
        self.cseq = 0
        self.buf = b""
        self.session_id = "1804289383"
        self.client_rtp_port = None
        self.rtp_ports_line = None
        self.gst = None
        self.state = "init"

    # ---------- Transport ----------
    def send_request(self, method, uri, headers=None, body=""):
        self.cseq += 1
        msg = "%s %s RTSP/1.0\r\nCSeq: %d\r\n" % (method, uri, self.cseq)
        for k, v in (headers or {}).items():
            msg += "%s: %s\r\n" % (k, v)
        if body:
            msg += "Content-Type: text/parameters\r\n"
            msg += "Content-Length: %d\r\n" % len(body)
        msg += "\r\n" + body
        log(">>>", msg.replace("\r\n", " | "))
        self.c.sendall(msg.encode())

    def send_response(self, cseq, headers=None):
        msg = "RTSP/1.0 200 OK\r\nCSeq: %s\r\n" % cseq
        for k, v in (headers or {}).items():
            msg += "%s: %s\r\n" % (k, v)
        msg += "\r\n"
        log(">>>", msg.replace("\r\n", " | "))
        self.c.sendall(msg.encode())

    def recv_message(self):
        while b"\r\n\r\n" not in self.buf:
            d = self.c.recv(4096)
            if not d:
                return None
            self.buf += d
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        head = head.decode(errors="replace")
        body = ""
        m = re.search(r"Content-Length:\s*(\d+)", head, re.I)
        if m:
            n = int(m.group(1))
            while len(self.buf) < n:
                d = self.c.recv(4096)
                if not d:
                    break
                self.buf += d
            body = self.buf[:n].decode(errors="replace")
            self.buf = self.buf[n:]
        log("<<<", head.replace("\r\n", " | "), "|BODY|", body.replace("\r\n", " | "))
        return head, body

    # ---------- Ablauf ----------
    def run(self):
        # Kurzes Timeout als Taktgeber: ein stiller Sink (der LG schweigt
        # nach PLAY komplett) darf NICHT zum Abbruch führen — wir müssen
        # trotzdem alle ~15 s ein Keepalive schicken, sonst beendet er die
        # Session exakt nach dem announcierten Session-Timeout (60 s).
        self.c.settimeout(5)
        # M1: OPTIONS Source -> Sink
        self.send_request("OPTIONS", "*", {"Require": "org.wfa.wfd1.0"})
        last_keepalive = time.time()
        last_msg = time.time()
        while True:
            try:
                msg = self.recv_message()
            except socket.timeout:
                now = time.time()
                if self.state == "playing":
                    if now - last_keepalive > 15:
                        self.send_request("GET_PARAMETER",
                                          "rtsp://localhost/wfd1.0",
                                          {"Session": self.session_id})
                        last_keepalive = now
                    continue
                if now - last_msg > 90:
                    log("!! no message from the sink for 90 s before PLAY, giving up (state %s)" % self.state)
                    break
                continue
            if msg is None:
                log("!! sink closed the connection (state %s)" % self.state)
                break
            last_msg = time.time()
            head, body = msg
            first = head.split("\r\n")[0]
            cseq = (re.search(r"CSeq:\s*(\d+)", head, re.I) or [None, "0"])[1]
            if first.startswith("RTSP/1.0"):
                self.handle_reply(first, head, body)
            else:
                method = first.split(" ")[0]
                self.handle_request(method, cseq, head, body)
            if self.state == "playing" and time.time() - last_keepalive > 15:
                self.send_request("GET_PARAMETER", "rtsp://localhost/wfd1.0",
                                  {"Session": self.session_id})
                last_keepalive = time.time()
            if self.state == "done":
                break

    def handle_reply(self, first, head, body):
        if "200" not in first:
            log("!! error reply in state %s:" % self.state, first)
            return
        if self.state == "init":
            # Antwort auf M1: erst auf M2 (OPTIONS vom Sink) warten, M3 kommt danach
            self.state = "await_m2"
        elif self.state == "m3sent":
            # M3-Antwort: RTP-Port des Sinks herausziehen
            m = re.search(r"wfd_client_rtp_ports:\s*(.+)", body)
            if m:
                self.rtp_ports_line = m.group(1).strip()
                p = re.search(r"(\d{3,5})", self.rtp_ports_line)
                if p:
                    self.client_rtp_port = int(p.group(1))
            log("== sink RTP port:", self.client_rtp_port)
            self.state = "m4sent"
            cea = os.environ.get("IMIRA_CEA", "00000020")  # 20=720p30, 80=1080p30
            ports = (self.rtp_ports_line or
                     "RTP/AVP/UDP;unicast %d 0 mode=play"
                     % (self.client_rtp_port or 1028))
            m4 = (
                "wfd_video_formats: 00 00 02 02 %s 00000000 00000000 00 0000 0000 11 none none\r\n"
                "wfd_audio_codecs: LPCM 00000002 00\r\n"
                "wfd_presentation_URL: rtsp://%s/wfd1.0/streamid=0 none\r\n"
                "wfd_client_rtp_ports: %s\r\n" % (cea, LOCAL_IP, ports))
            self.send_request("SET_PARAMETER", "rtsp://localhost/wfd1.0", {}, m4)
        elif self.state == "m4sent":
            # M4 bestaetigt -> M5: Trigger SETUP
            self.state = "m5sent"
            self.send_request("SET_PARAMETER", "rtsp://localhost/wfd1.0", {},
                              "wfd_trigger_method: SETUP\r\n")
        elif self.state == "m5sent":
            log("== M5 acknowledged, waiting for SETUP from the sink")

    def handle_request(self, method, cseq, head, body):
        if method == "OPTIONS":  # M2
            self.send_response(cseq, {
                "Public": "org.wfa.wfd1.0, GET_PARAMETER, SET_PARAMETER, "
                          "SETUP, PLAY, PAUSE, TEARDOWN"})
            if self.state in ("init", "await_m2"):
                self.state = "m3sent"
                self.send_request(
                    "GET_PARAMETER", "rtsp://localhost/wfd1.0", {},
                    "wfd_video_formats\r\nwfd_audio_codecs\r\n"
                    "wfd_client_rtp_ports\r\nwfd_content_protection\r\n")
        elif method == "SETUP":  # M6
            m = re.search(r"client_port=(\d+)", head)
            if m:
                self.client_rtp_port = int(m.group(1))
            log("== SETUP, client_port =", self.client_rtp_port)
            self.send_response(cseq, {
                "Session": "%s;timeout=60" % self.session_id,
                "Transport": "RTP/AVP/UDP;unicast;client_port=%d;server_port=%d-%d"
                             % (self.client_rtp_port, SERVER_RTP, SERVER_RTP + 1)})
        elif method == "PLAY":  # M7
            self.send_response(cseq, {"Session": self.session_id})
            self.start_stream()
            self.state = "playing"
        elif method == "TEARDOWN":  # M8
            self.send_response(cseq, {"Session": self.session_id})
            self.stop_stream()
            self.state = "done"
        elif method in ("GET_PARAMETER", "SET_PARAMETER"):
            self.send_response(cseq, {"Session": self.session_id})
            if "wfd_idr_request" in body:
                # Keyframe-Wunsch an den Encoder weiterreichen (ratenbegrenzt)
                now = time.time()
                if now - getattr(self, "_last_idr_fwd", 0) > 0.7:
                    self._last_idr_fwd = now
                    subprocess.call(["pkill", "-USR1", "-x", "imira-castd"])
        else:
            log("?? unknown method", method)
            self.send_response(cseq)

    # ---------- Stream ----------
    def start_stream(self):
        if self.gst:
            return
        # IMIRA_STREAM_CMD (mit {ip}/{port}) erlaubt z.B. den imira-castd:
        #   IMIRA_STREAM_CMD="/opt/imira/imira-castd --dest {ip} --port {port}"
        cmd = os.environ.get("IMIRA_STREAM_CMD")
        if cmd:
            cmd = cmd.format(ip=SINK_IP, port=self.client_rtp_port)
        else:
            cmd = ("while true; do gst-launch-1.0 -q filesrc location=%s "
                   "! tsparse set-timestamps=true ! rtpmp2tpay "
                   "! udpsink host=%s port=%d bind-port=%d sync=true; done"
                   % (TS_FILE, SINK_IP, self.client_rtp_port, SERVER_RTP))
        log("== starting RTP stream ->", "%s:%d" % (SINK_IP, self.client_rtp_port))
        self.gst = subprocess.Popen(["/bin/bash", "-c", cmd])

    def stop_stream(self):
        if self.gst:
            subprocess.call(["/bin/bash", "-c",
                             "kill %d 2>/dev/null" % self.gst.pid])
            subprocess.call(["pkill", "-x", "gst-launch-1.0"])
            subprocess.call(["pkill", "-x", "imira-castd"])
            self.gst = None


# Ein Sink, der die Gruppe steht, verbindet sich binnen Sekunden. Wartet er
# länger, kommt er nicht mehr — dann lieber neu verbinden, als minutenlang
# "Warte auf Empfänger" zu zeigen.
RTSP_WAIT = int(os.environ.get("IMIRA_RTSP_WAIT", "60"))


def main():
    global LOCAL_IP, SINK_IP
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", RTSP_PORT))
    s.listen(1)
    s.settimeout(RTSP_WAIT)
    log("== waiting for the sink's RTSP connection on port %d (local %s, sink %s)"
        % (RTSP_PORT, LOCAL_IP, SINK_IP))
    t0 = time.time()
    try:
        conn, addr = s.accept()
    except socket.timeout:
        log("!! the sink did not open an RTSP connection within %d s" % RTSP_WAIT)
        return
    conn.settimeout(None)
    log("== sink connected from %s:%d after %.1f s" % (addr[0], addr[1], time.time() - t0))
    # Die tatsächlichen Adressen dieser Verbindung gelten, nicht die aus dem
    # Gruppen-Ereignis erschlossenen: RTP geht dorthin, woher der Sink kam.
    local = conn.getsockname()[0]
    if addr[0] != SINK_IP or local != LOCAL_IP:
        log("== using connection addresses: local %s (expected %s), sink %s (expected %s)"
            % (local, LOCAL_IP, addr[0], SINK_IP))
    LOCAL_IP, SINK_IP = local, addr[0]
    sess = WFDSession(conn)

    def graceful(_sig, _frm):
        # Dem Sink ein sauberes Sitzungsende melden, sonst bleibt z.B. der
        # LG-TV minutenlang auf dem letzten Frame stehen.
        log("== SIGTERM: sending TEARDOWN to the sink")
        try:
            sess.send_request("TEARDOWN", "rtsp://localhost/wfd1.0/streamid=0",
                              {"Session": sess.session_id})
            time.sleep(0.3)
        except Exception:
            pass
        sess.stop_stream()
        os._exit(0)

    signal.signal(signal.SIGTERM, graceful)
    try:
        sess.run()
    finally:
        sess.stop_stream()
    log("== end, state:", sess.state)


if __name__ == "__main__":
    main()
