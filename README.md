<img src="appicon/harbour-imira.svg" width="96" align="left" alt="Imira icon">

# Imira — Miracast screen mirroring for Sailfish OS

Mirrors the phone screen **and audio** to a Miracast sink — a TV or an HDMI
dongle — over Wi-Fi Direct. No app on the TV, no cables, no network required:
the phone talks to the sink directly.

<br clear="left">

As far as we know this is the first working Wi-Fi Display *source* for
Sailfish OS. Developed and tested on the **Xperia 10 III** (Sailfish OS
5.0.0.62) against a Microsoft Wireless Display Adapter V2 and an LG webOS TV
(native Miracast), using the phone's internal Wi-Fi chip.

## Features

- **Video**: screen capture via the Lipstick recorder interface, hardware
  H.264 encoding (droidmedia), MPEG-TS/RTP streaming. Live switch between
  720p and 1080p while casting.
- **Audio**: the phone's media output travels with the cast (LPCM). While
  casting, media playback is silenced locally — sound comes out of the TV
  only, and the volume keys control the TV loudness. Ringtones, alarms and
  calls deliberately stay on the phone. When the cast ends, playback returns
  to the phone speaker by itself.
- **Privacy by construction**: the capture verifies it reads a sink
  *monitor* and refuses anything else — the microphone can never leak into
  the stream. (Sailfish's audio policy actively tries to hand record streams
  the mic; see `device/imira-xpolicy.conf` for the counter-measure.)
- **Orientation**: automatic compensation keeps the TV image upright while
  the phone rotates; alternatively pin portrait or either landscape
  direction — for fullscreen games that use the gyroscope.
- **A/V sync**: live-adjustable audio offset (±2 s) from the app, applied
  without interrupting the cast.
- Device scan with sink selection, cast start/stop from the app, closing the
  app ends the cast.

## How it works

```
app (Silica UI)  ⇄  flag files in /tmp  ⇄  imira.service (root)
                                             ├─ bundled P2P wpa_supplicant
                                             │  (Wi-Fi Direct group, see vendor/)
                                             ├─ imira-wfd-proto.py
                                             │  (WFD RTSP handshake M1–M8)
                                             └─ imira-castd (user session)
                                                capture → convert/rotate →
                                                HW H.264 → MPEG-TS → RTP/UDP
```

The stock Sailfish `wpa_supplicant` has P2P compiled out, so the service
ships its own build and runs it alongside the system one — nothing
system-wide is replaced. `vendor/README.md` has the one-line patch and the
recipe.

## Building

Three parts, built separately (Sailfish Platform SDK):

```sh
# 1. the streaming daemon (sb2 cross build, result: daemon/imira-castd)
daemon/build-castd.sh

# 2. the bundled P2P wpa_supplicant — see vendor/README.md

# 3. the app + packaging
mb2 -t SailfishOS-5.0.0.62-aarch64 build
```

## Status

Working end to end on the hardware above; other devices untested. Known
rough edge: playing videos in the Gallery **while casting** can crash the
Gallery's playback (hardware encoder and decoder share the video core; the
droid stack does not always survive that — other players cope better).

Planned next: convergence — keyboard/mouse on the phone, the TV as a real
second screen with dockable windows.

## License

GPL-3.0-or-later. Parts of the Wi-Fi Direct and screen recording approach
are derived from the aethercast and screencast projects (GPL).
