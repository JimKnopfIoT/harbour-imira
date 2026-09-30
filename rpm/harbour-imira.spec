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
Version:    0.10.10
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
install -m 0755 device/imira-dhcp.sh      %{buildroot}/usr/libexec/imira/
install -m 0755 device/imira-report.py    %{buildroot}/usr/libexec/imira/
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
%attr(0755,root,root) /usr/libexec/imira/imira-dhcp.sh
%attr(0755,root,root) /usr/libexec/imira/imira-report.py
%attr(0644,root,root) /usr/libexec/imira/wpa-imira.conf.in
%attr(0755,root,root) /usr/libexec/imira/wpa_supplicant-p2p
%attr(0755,root,root) /usr/libexec/imira/wpa_cli-p2p
%attr(0755,root,root) /usr/libexec/imira/run-castd.sh
%attr(0644,root,root) %{_sysconfdir}/systemd/system/imira.service
%attr(0644,root,root) %{_datadir}/polkit-1/rules.d/50-imira.rules
%attr(0644,root,root) %{_sysconfdir}/pulse/xpolicy.conf.d/imira.conf

%changelog
* Wed Sep 30 2026 harbour-imira contributors 0.10.10-1
- Sound in AAC: some receivers (a hichip projector) offer uncompressed
  LPCM sound but play only AAC, so the picture came and the sound did not.
  Imira now sends AAC whenever the receiver offers it and the phone can
  encode it (Android's own encoder, checked once at the first cast).
- New "Sound format" setting on the main page: Automatic, AAC or LPCM.
  If a receiver shows the picture but plays no sound, try the other one.
  Takes effect on the next cast.
- The diagnostic report names the sound format that was used.
* Tue Sep 29 2026 harbour-imira contributors 0.10.9-1
- Imira now listens for the receiver before the phone takes its address in
  the Wi-Fi Direct group. Some receivers (a hichip projector) try to
  connect once, right after handing out the address; if nothing listened
  at that moment they never came back, and most casts ended without a
  picture.
- A receiver that names an address outside its own network as its DHCP
  server no longer confuses the connection check; the first address of
  the network is used instead.
- The diagnostic report fits into a forum post: the most useful parts come
  first, long logs are shortened to stay below the forum's size limit, and
  hidden values show as [ip] instead of <ip>, which the forum swallowed.
* Tue Sep 29 2026 harbour-imira contributors 0.10.8-1
- New "Audio route" button (main page, and on the cover while streaming):
  steps through Automatic, Everything to the TV (Bluetooth, ringtones and
  streams of unknown kinds too) and a direct copy of every audio output the
  phone has. It applies live, so it can be tried while a video plays.
- Automatic now takes streams from every output of the phone. The Jolla
  Phone 2026 has a third one (sink.fast); sound playing there stayed on
  the phone speaker and the TV got silence.
- After a cast the phone plays its music again. The Jolla Phone 2026 sends
  sound whose output disappears to a null output, so a player that kept
  playing through the end of the cast went mute. Streams are now moved back
  to where they came from, within a fraction of a second.
- The diagnostic report has an audio log: the phone's outputs, every stream
  with app, kind and output and whether it went to the TV (and why not),
  every route switch, and every 5 s whether the TV gets sound or silence.
  Bluetooth addresses in output names are anonymized.

* Mon Sep 28 2026 harbour-imira contributors 0.10.7-1
- Casting no longer fails at once with "Failed to create interface
  p2p-p2p0-0: -22". The Wi-Fi driver of the Jolla Phone 2026 allows only
  two extra network interfaces; interfaces left over from an earlier
  Wi-Fi Direct group could take both, and no new group could be made.
  They are now removed before each cast.
- The diagnostic report lists the phone's wireless interfaces with their
  types.
- The connect log no longer reports 0 frames for short sessions that did
  send pictures.

* Mon Sep 28 2026 harbour-imira contributors 0.10.6-1
- Casting works on phones as they ship. The session script counted time
  with a bash feature ($SECONDS) that the phone's default shell, busybox,
  does not have; the service stopped right after the receiver's Wi-Fi
  Direct connection was up, before the receiver could start the session,
  and was restarted. For the same reason the service did not end by itself
  after the app was closed, but was started again and again. Development
  phones with GNU bash installed never showed it.

* Sun Sep 27 2026 harbour-imira contributors 0.10.5-1
- The picture no longer goes black after a few seconds, which it did with
  0.10.4 on the Jolla Phone 2026. The audio capture had stopped asking
  PulseAudio for a latency; with nothing else asking, PulseAudio then
  handed the sound over in two-second lumps whose timestamps ran up to
  three seconds ahead, and the receiver, which keys the picture to the
  audio clock, threw every frame away.
- A new audio clock. The timeline follows the sample position, anchored on
  arrival times instead of PulseAudio's latency figure, which wobbles
  around zero and read 711 ms for the first blocks. The old clock jumped by
  a quarter of a second whenever that figure drifted — heard as the sound
  swallowing itself every few seconds, and after turning the phone as a
  black picture. The new one was designed on recorded data and moves by at
  most a few milliseconds a second.
- Sound and picture get a presentation margin of 150 ms, so the audio
  reaches the receiver before it is due instead of being dropped as late.
- One radio, one channel. When the phone is connected to a Wi-Fi network,
  the cast now asks the receiver for that network's channel, so the radio
  no longer has to hop between two channels and lose packets on the way —
  measured as the main cause of remaining dropouts. If the network uses a
  radar channel (5 GHz, 52–144) that Wi-Fi Direct may not share, or the
  receiver declines, the app says so and suggests a 2.4 GHz network or
  disconnecting while casting.
- Diagnostics page (About → Diagnostics) for receivers nobody has tested
  yet. A detailed log records the whole Wi-Fi Direct negotiation, and a
  report collects everything needed to find out why a cast fails: a fresh
  search for receivers with what each one says about itself, the connect
  and handshake logs, the relevant part of the supplicant log, the phone's
  wireless capabilities, the Wi-Fi channel and, optionally, the channels in
  use around it. The report is anonymized — MAC addresses cut to the maker
  part, names of other devices and networks, serial numbers and public
  addresses removed — and lands in Documents, ready to be copied into a
  forum post.
- Receivers that hand out addresses by DHCP now work. The address used to
  come only from the Wi-Fi Direct handshake itself, a newer feature many
  TVs lack; without it the TV never learned where to connect.
- "Streaming" now means that pictures are actually going out. Until the
  receiver has set up the session, the app says "Waiting for receiver".
  A receiver that never opens the session is given up on after a minute,
  and such an attempt counts as failed instead of being retried forever.
- Every log is in English and time-stamped, and says much more: the
  receiver's details per attempt, why the supplicant gave up, where the
  address came from, which channels are in use, and what goes in and comes
  out of the encoder.

* Sun Sep 13 2026 harbour-imira contributors 0.10.4-1
- The picture no longer starves the sound. While mirroring, the screen is
  only captured when something on it changes, and a quiet screen was nudged
  just twice a second — so the sink received video timestamps up to three
  quarters of a second apart. Sinks tie their audio output to that clock,
  and it was heard as stuttering music, not as a stuttering picture. The
  nudge now keeps the frame rate steady, which also lets the encoder budget
  its bits normally instead of spending a whole second's worth on the rare
  frame it got: the picture is sharp immediately after a change of
  orientation instead of mushy for a moment.
- The programme clock is sent independently of the video. It used to ride
  along on video frames, which is fine at thirty frames a second and far
  outside spec at two.
- Frames are paced by deadline rather than by a minimum gap. A minimum gap
  discards a frame that arrives a hair early and then waits a whole period
  for the next one, halving the rate; measured on one phone, twenty-seven
  frames a second arrived and fourteen went out.
- Colour conversion is roughly two and a half times faster: the source
  coordinates are computed once per geometry instead of once per pixel — a
  64-bit division for every pixel of every frame — and the rows are now
  converted on several cores, which yield to the audio path.
- A large frame is sent in slices, so audio packets are no longer queued
  behind a third of a megabyte.
- Audio capture asks for a bigger fragment and no longer demands minimal
  latency, which had forced the silencing sink down to a ten millisecond
  deadline. Its timeline is placed per block instead of per chunk and only
  re-anchors on real drift, not on ordinary jitter.
- The daemon reports gaps, timeline jumps and silent stretches in the
  captured audio, which turns "it stutters sometimes" into a number.

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
