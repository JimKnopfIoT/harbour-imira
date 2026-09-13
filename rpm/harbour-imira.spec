# harbour-imira — Miracast screen mirroring for Sailfish OS.
#
# Two halves in one package:
#   1. The Silica UI app, built here the usual %%qmake5 way.
#   2. The casting machinery under /usr/libexec/imira/ — daemon/imira-castd
#      and vendor-wpa/wpa_supplicant are built OUTSIDE this spec (see
#      daemon/build-castd.sh and vendor/wpa_supplicant-p2p/) and are only
#      copied out of the source tree in %%install. Do not add build steps for
#      them here.
#
# Neutral packaging metadata — no personal identifiers (anonymity rules).
Name:       harbour-imira
Summary:    Miracast screen mirroring for Sailfish OS
Version:    0.10.3
Release:    1
# ANONYMITY: neutral build host so built RPMs carry no real hostname/domain.
%define _buildhost reproducible-builder
License:    GPL-3.0-or-later
URL:        https://github.com/JimKnopfIoT/harbour-imira
Source0:    %{name}-%{version}.tar.bz2
Vendor:     harbour-imira contributors
Packager:   harbour-imira contributors

Requires:   sailfishsilica-qt5
# The WFD RTSP handshake helper (imira-wfd-proto.py) runs under python3.
# Requires stays minimal on purpose: everything else the service uses
# (systemd, iw, udhcpd/dhcp) is part of the stock image.
Requires:   python3-base

BuildRequires: pkgconfig(sailfishapp)
BuildRequires: pkgconfig(Qt5Core)
BuildRequires: pkgconfig(Qt5Qml)
BuildRequires: pkgconfig(Qt5Quick)
BuildRequires: desktop-file-utils
# lrelease for CONFIG+=sailfishapp_i18n
BuildRequires: qt5-qttools-linguist

# imira-castd links against libdroidmedia.so, which the droidmedia package
# ships without rpm provides metadata; the bundled wpa_supplicant similarly
# drags in library sonames that must not become hard package requirements.
# Everything under /usr/libexec/imira is prebuilt and vetted by hand, so
# switch off automatic requires for the package.
AutoReq: 0
AutoProv: 0

%description
Mirrors the phone screen to a Miracast (Wi-Fi Display) sink such as a TV or
an HDMI dongle. The heavy lifting — Wi-Fi Direct group setup via a bundled
wpa_supplicant, the WFD RTSP handshake, screen capture through the Lipstick
recorder interface and H.264/RTP streaming — is done by a root systemd
service; the app is a thin remote control that talks to it through flag
files in /tmp.

Parts of the Wi-Fi Direct and screen recording approach are derived from the
aethercast and screencast projects (GPL).

%prep
%setup -q

%build
# VERSION/RELEASE reach the About page through DEFINES in harbour-imira.pro.
%qmake5 VERSION=%{version} RELEASE=%{release}
%make_build

%install
%qmake5_install

desktop-file-install --delete-original \
    --dir %{buildroot}%{_datadir}/applications \
    %{buildroot}%{_datadir}/applications/*.desktop

# --- casting machinery (prebuilt, copied from the source tree) --------------
install -d %{buildroot}/usr/libexec/imira

# Screen recorder + encoder + RTP sender. Built by daemon/build-castd.sh.
install -m 0755 daemon/imira-castd %{buildroot}/usr/libexec/imira/imira-castd

# Convergence compositor (virtual TV screen). Built by daemon/comp/build-comp.sh.
install -m 0755 daemon/comp/imira-comp %{buildroot}/usr/libexec/imira/imira-comp

# Service-side scripts and the WFD RTSP protocol helper.
install -m 0755 device/imira-session.sh   %{buildroot}/usr/libexec/imira/
install -m 0755 device/imira-connect.sh   %{buildroot}/usr/libexec/imira/
install -m 0755 device/imira-wfd-proto.py %{buildroot}/usr/libexec/imira/
install -m 0644 device/wpa-imira.conf.in  %{buildroot}/usr/libexec/imira/

# Bundled P2P-capable wpa_supplicant (the stock one has P2P compiled out).
# Built separately under vendor/wpa_supplicant-p2p/.
install -m 0755 vendor/wpa_supplicant-p2p/extracted/usr/sbin/wpa_supplicant \
    %{buildroot}/usr/libexec/imira/wpa_supplicant-p2p
install -m 0755 vendor/wpa_supplicant-p2p/extracted/usr/sbin/wpa_cli \
    %{buildroot}/usr/libexec/imira/wpa_cli-p2p
install -m 0755 device/run-castd.sh %{buildroot}/usr/libexec/imira/

# Not enabled at boot: app-started (polkit rule below), exits with the app.
install -d %{buildroot}%{_sysconfdir}/systemd/system
install -m 0644 device/imira.service \
    %{buildroot}%{_sysconfdir}/systemd/system/imira.service
install -d %{buildroot}%{_datadir}/polkit-1/rules.d
install -m 0644 device/50-imira.rules \
    %{buildroot}%{_datadir}/polkit-1/rules.d/50-imira.rules

# Audio-policy exception: keeps the daemon's monitor capture off the mic.
install -d %{buildroot}%{_sysconfdir}/pulse/xpolicy.conf.d
install -m 0644 device/imira-xpolicy.conf \
    %{buildroot}%{_sysconfdir}/pulse/xpolicy.conf.d/imira.conf

%post
# Installed straight into /etc/systemd/system, so only a reload is needed.
# Guarded: during image builds there is no running systemd.
systemctl daemon-reload >/dev/null 2>&1 || :
# Upgrades: disable the pre-0.10.1 boot service.
if [ "$1" -gt 1 ]; then
    systemctl disable --now imira.service >/dev/null 2>&1 || :
fi
# PulseAudio only reads xpolicy.conf.d on startup; kick the user instance
# (systemd --user respawns it immediately). Without the rule the daemon's
# monitor capture would be rerouted to the mic — which it refuses, so audio
# would stay off until the next reboot.
pkill -x pulseaudio >/dev/null 2>&1 || :

%preun
# $1 = 0 on erase, >= 1 on upgrade — only stop the service when going away.
if [ "$1" = "0" ]; then
    systemctl stop imira.service >/dev/null 2>&1 || :
    systemctl disable imira.service >/dev/null 2>&1 || :
fi

%postun
systemctl daemon-reload >/dev/null 2>&1 || :

%files
%defattr(-,root,root,-)
%{_bindir}/%{name}
%{_datadir}/%{name}
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png

%dir /usr/libexec/imira
# setgid privileged: the Lipstick recorder socket is only reachable for the
# privileged group; without the 2 bit the daemon records nothing.
%attr(2755,root,privileged) /usr/libexec/imira/imira-castd
%attr(2755,root,privileged) /usr/libexec/imira/imira-comp
%attr(0755,root,root) /usr/libexec/imira/imira-session.sh
%attr(0755,root,root) /usr/libexec/imira/imira-connect.sh
%attr(0755,root,root) /usr/libexec/imira/imira-wfd-proto.py
%attr(0644,root,root) /usr/libexec/imira/wpa-imira.conf.in
%attr(0755,root,root) /usr/libexec/imira/wpa_supplicant-p2p
%attr(0755,root,root) /usr/libexec/imira/wpa_cli-p2p
%attr(0755,root,root) /usr/libexec/imira/run-castd.sh
%attr(0644,root,root) %{_sysconfdir}/systemd/system/imira.service
%attr(0644,root,root) %{_datadir}/polkit-1/rules.d/50-imira.rules
%attr(0644,root,root) %{_sysconfdir}/pulse/xpolicy.conf.d/imira.conf

%changelog
* Sun Sep 13 2026 harbour-imira contributors 0.10.3-1
- Fixes an upgrade trap in 0.10.2: the session address is now removed from
  the base interface before it is put on the group interface. Older versions
  ran the group on the base interface and left the address behind, and that
  leftover kept the route pointing at an interface that is down — the sink
  connected, got no answer and dropped the group after fifteen seconds.

* Sun Sep 13 2026 harbour-imira contributors 0.10.2-1
- Works on MediaTek phones. The Wi-Fi Direct group now runs on an interface
  wpa_supplicant creates itself; the built-in p2p0 of a MediaTek chip accepts
  discovery and group negotiation but never an association, so every cast
  failed there right after the peer said yes.
- A resolution change no longer kills the encoder. The frame was still built
  in the old geometry and then handed to the freshly created encoder, which
  overran its input buffer — fatal on MediaTek, survivable on Qualcomm.
- Connection events are read from the supplicant's own log instead of the
  journal, which on some devices is volatile, tiny and rate limited, and
  silently dropped the very lines the connect step waits for.
- Only real Wi-Fi Display sinks are offered as targets. A second phone
  running this app announces the same Wi-Fi Display information as a source
  and used to be picked as a destination.
- The convergence screen stays put when the phone is turned. It is a
  landscape desktop of its own; following the sensor belongs to mirroring.
- The convergence picture is no longer thinned out on its way to the sink.
  The frame reader slept a full frame period after each frame instead of
  between frames, and polled in the compositor's own rhythm, so barely more
  than a third of the rendered frames arrived — the cursor stuttered.

* Thu Aug 20 2026 harbour-imira contributors 0.10.1-1
- The session service no longer runs at boot. The app starts it on launch
  (StartUnit over the system bus, polkit rule scoped to imira.service) and
  it exits by itself once the app's heartbeat stops. Upgrades disable and
  stop the old boot service.

* Sun Aug 16 2026 harbour-imira contributors 0.10.0-1
- Convergence desktop: window management (move, resize grip, minimize,
  maximize, close, stacking, opaque backdrops), dock as task bar with
  live-reloaded per-user app selection, external keyboard/mouse with
  Bluetooth-reconnect resilience and correct keycodes, per-app content
  orientation compensation, no on-screen keyboard on the TV, TV view page
  on the phone (opt-in live preview, per-process convergence load monitor),
  pixel-perfect TV screenshots via Print key or the app.
* Sat Aug 15 2026 harbour-imira contributors 0.1.0-1
- Initial skeleton: Silica UI (status page, cover, German translation),
  flag-file CastController, packaging of the externally built casting
  service parts under /usr/libexec/imira.
