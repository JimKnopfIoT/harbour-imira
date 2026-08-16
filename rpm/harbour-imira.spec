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
Version:    0.9.0
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

install -d %{buildroot}%{_sysconfdir}/systemd/system
install -m 0644 device/imira.service \
    %{buildroot}%{_sysconfdir}/systemd/system/imira.service

# Audio-policy exception: keeps the daemon's monitor capture off the mic.
install -d %{buildroot}%{_sysconfdir}/pulse/xpolicy.conf.d
install -m 0644 device/imira-xpolicy.conf \
    %{buildroot}%{_sysconfdir}/pulse/xpolicy.conf.d/imira.conf

%post
# Installed straight into /etc/systemd/system, so only a reload is needed.
# Guarded: during image builds there is no running systemd.
systemctl daemon-reload >/dev/null 2>&1 || :
systemctl enable imira.service >/dev/null 2>&1 || :
systemctl start imira.service >/dev/null 2>&1 || :
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
%attr(0755,root,root) /usr/libexec/imira/imira-comp
%attr(0755,root,root) /usr/libexec/imira/imira-session.sh
%attr(0755,root,root) /usr/libexec/imira/imira-connect.sh
%attr(0755,root,root) /usr/libexec/imira/imira-wfd-proto.py
%attr(0644,root,root) /usr/libexec/imira/wpa-imira.conf.in
%attr(0755,root,root) /usr/libexec/imira/wpa_supplicant-p2p
%attr(0755,root,root) /usr/libexec/imira/wpa_cli-p2p
%attr(0755,root,root) /usr/libexec/imira/run-castd.sh
%attr(0644,root,root) %{_sysconfdir}/systemd/system/imira.service
%attr(0644,root,root) %{_sysconfdir}/pulse/xpolicy.conf.d/imira.conf

%changelog
* Sat Aug 15 2026 harbour-imira contributors 0.1.0-1
- Initial skeleton: Silica UI (status page, cover, German translation),
  flag-file CastController, packaging of the externally built casting
  service parts under /usr/libexec/imira.
