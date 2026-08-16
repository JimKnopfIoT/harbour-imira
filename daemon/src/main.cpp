/*
 * imira-castd — Wi-Fi Display sender core for harbour-imira.
 *
 * Screen (lipstick-recorder) -> FrameConverter (RGBA->YUV420) ->
 * H264Encoder (droidmedia HW) -> TsMux (MPEG-TS) -> RtpSender (UDP).
 *
 * The WFD RTSP session (M1-M8) is handled by the controlling process;
 * this daemon only produces the RTP media stream.
 */
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

#include "audiocapture.h"
#include "convert.h"
#include "encoder.h"
#include "orientation.h"
#include "recorder.h"
#include "rtpsender.h"
#include "tsmux.h"

using namespace imira;

namespace {

std::atomic<bool> g_running{true};
std::atomic<bool> g_needIdr{false};

// Exit immediately: waiting for the wayland dispatch thread can hang forever,
// and a half-dead instance keeps the compositor's recorder slot occupied.
// The kernel/mediaserver reclaim sockets and codec on process death.
void onSignal(int) { _exit(0); }

void onIdrRequest(int) { g_needIdr = true; }

int64_t nowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Options {
    std::string dest;
    int port = 19000;
    int width = 1280;
    int height = 720;
    int fps = 30;
    int bitrate = 6000000;
};

bool parseArgs(int argc, char **argv, Options &o)
{
    for (int i = 1; i < argc - 1; i++) {
        std::string a = argv[i];
        if (a == "--dest") o.dest = argv[++i];
        else if (a == "--port") o.port = atoi(argv[++i]);
        else if (a == "--width") o.width = atoi(argv[++i]);
        else if (a == "--height") o.height = atoi(argv[++i]);
        else if (a == "--fps") o.fps = atoi(argv[++i]);
        else if (a == "--bitrate") o.bitrate = atoi(argv[++i]);
    }
    return !o.dest.empty();
}

} // namespace

int main(int argc, char **argv)
{
    Options opt;
    if (!parseArgs(argc, argv, opt)) {
        fprintf(stderr,
                "usage: imira-castd --dest <ip> [--port 19000] [--width 1280]"
                " [--height 720] [--fps 30] [--bitrate 6000000]\n");
        return 1;
    }
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    signal(SIGUSR1, onIdrRequest); // sink sent wfd_idr_request

    RtpSender rtp;
    if (!rtp.open(opt.dest, (uint16_t)opt.port, (uint16_t)(opt.port))) {
        fprintf(stderr, "imira-castd: cannot open RTP socket\n");
        return 1;
    }

    TsMux mux;
    mux.addH264Track(opt.width, opt.height, opt.fps, 1);
    const char *audioSrcEnv = getenv("IMIRA_AUDIO_SOURCE");
    const bool audioOn = !(audioSrcEnv && std::string(audioSrcEnv) == "off");
    if (audioOn)
        mux.addLpcmTrack(48000, 2);

    std::mutex sendLock;
    int64_t lastPatUs = 0;

    // Debug taps: IMIRA_DUMP_H264 / IMIRA_DUMP_TS write the elementary
    // stream / mux output to files for offline analysis.
    FILE *dumpH264 = nullptr, *dumpTs = nullptr;
    if (const char *p = getenv("IMIRA_DUMP_H264"))
        dumpH264 = fopen(p, "wb");
    if (const char *p = getenv("IMIRA_DUMP_TS"))
        dumpTs = fopen(p, "wb");
    int logged = 0;

    H264Encoder enc;
    bool ok = enc.init(opt.width, opt.height, opt.fps, opt.bitrate,
        [&](const uint8_t *data, size_t size, int64_t ptsUs, bool idr,
            bool codecConfig) {
            // droidmedia timestamps are nanoseconds on both sides.
            ptsUs /= 1000;
            // The codec's sync flag does not reach us reliably; detect IDR
            // access units by NAL type (prepended SPS (7) or IDR slice (5)).
            if (!idr && size > 4) {
                int nal = data[4] & 0x1f;
                idr = (nal == 7 || nal == 5);
            }
            std::lock_guard<std::mutex> l(sendLock);
            if (logged < 6) {
                fprintf(stderr,
                        "imira-castd: AU size=%zu pts=%lld idr=%d cfg=%d "
                        "head=%02x%02x%02x%02x%02x%02x\n",
                        size, (long long)ptsUs, idr, codecConfig,
                        size > 0 ? data[0] : 0, size > 1 ? data[1] : 0,
                        size > 2 ? data[2] : 0, size > 3 ? data[3] : 0,
                        size > 4 ? data[4] : 0, size > 5 ? data[5] : 0);
                logged++;
            }
            if (dumpH264) {
                fwrite(data, 1, size, dumpH264);
                fflush(dumpH264);
            }
            if (codecConfig) {
                mux.setCodecConfig(data, size);
                return;
            }
            int64_t t = nowUs();
            bool withPat = idr || (t - lastPatUs) > 100000;
            if (withPat)
                lastPatUs = t;
            std::vector<uint8_t> ts;
            if (mux.packetize(data, size, ptsUs, idr, withPat, ts)) {
                if (dumpTs) {
                    fwrite(ts.data(), 1, ts.size(), dumpTs);
                    fflush(dumpTs);
                }
                rtp.send(ts.data(), ts.size(), t);
            }
        });
    if (!ok) {
        fprintf(stderr, "imira-castd: encoder init failed: %s\n",
                enc.lastError().c_str());
        return 1;
    }

    // A/V lip-sync trim: added to every audio PTS. Negative = audio earlier.
    // The old -400 ms default was calibrated against the microphone path
    // (policy had silently rerouted the capture stream); the true monitor
    // starts at 0. Live-tunable via /tmp/imira-audio-offset (milliseconds),
    // fed by the slider in the app.
    std::atomic<long long> audioOffsetUs{0};

    AudioCapture audio;
    if (audioOn) {
        audio.start(audioSrcEnv ? audioSrcEnv : "",
                    [&](const uint8_t *pcm, size_t size, int64_t ptsUs) {
            std::lock_guard<std::mutex> l(sendLock);
            std::vector<uint8_t> ts;
            if (mux.packetizeAudio(pcm, size, ptsUs + audioOffsetUs.load(), ts)) {
                if (dumpTs) {
                    fwrite(ts.data(), 1, ts.size(), dumpTs);
                    fflush(dumpTs);
                }
                rtp.send(ts.data(), ts.size(), nowUs());
            }
        });
    }

    FrameConverter conv;
    conv.configure(opt.width, opt.height,
                   enc.inputFormat() == H264Encoder::NV12);

    const int64_t frameIntervalUs = 1000000 / opt.fps;
    std::atomic<int64_t> lastQueuedUs{0};
    std::atomic<long> frames{0}, drops{0};
    // Content rotation (0/90/270): the orientation sensor keeps its own
    // value current; a value in /tmp/imira-rotate (polled below) overrides
    // it manually. Two separate atomics — the sensor must never fight the
    // override (it used to win for up to one poll interval per turn).
    std::atomic<int> sensorRotation{0};
    std::atomic<int> overrideRotation{-1}; // -1 = automatic (sensor)
    startOrientationWatcher(&sensorRotation);
    // Live resolution switch via /tmp/imira-res ("720"/"1080"): applied on
    // the next captured frame through an encoder restart with new size.
    std::atomic<int> pendingW{0}, pendingH{0}, pendingBr{0};

    ScreenRecorder rec;
    ok = rec.start([&](const uint8_t *pixels, int width, int height,
                       int stride, uint32_t /*drmFormat*/, int transform) {
        int64_t t = nowUs();
        if (t - lastQueuedUs.load() < frameIntervalUs) {
            drops++;
            return; // stay at the target frame rate
        }
        int ovr = overrideRotation.load();
        size_t size = 0;
        uint8_t *frame = conv.convert(pixels, width, height, stride,
                                      transform == 2 /* y_inverted */,
                                      ovr >= 0 ? ovr : sensorRotation.load(),
                                      &size);
        if (!frame)
            return;
        int pw = pendingW.exchange(0);
        if (pw) {
            int ph = pendingH.load(), pb = pendingBr.load();
            fprintf(stderr, "imira-castd: switching to %dx%d @%d\n", pw, ph, pb);
            if (enc.restartWith(pw, ph, pb)) {
                conv.configure(pw, ph, enc.inputFormat() == H264Encoder::NV12);
                opt.width = pw;
                opt.height = ph;
            } else {
                fprintf(stderr, "imira-castd: resolution switch FAILED\n");
            }
        } else if (g_needIdr.exchange(false)) {
            fprintf(stderr, "imira-castd: IDR requested, restarting encoder\n");
            if (!enc.restart())
                fprintf(stderr, "imira-castd: encoder restart FAILED\n");
        }
        lastQueuedUs = t;
        frames++;
        // droidmedia takes microseconds in (MediaCodec convention) but
        // reports nanoseconds out — the output path divides by 1000.
        enc.queueFrame(frame, size, t);
    });
    if (!ok) {
        fprintf(stderr, "imira-castd: cannot start screen recorder "
                        "(WAYLAND_DISPLAY/XDG_RUNTIME_DIR correct?)\n");
        enc.stop();
        return 1;
    }

    fprintf(stderr, "imira-castd: streaming %dx%d@%d -> %s:%d\n", opt.width,
            opt.height, opt.fps, opt.dest.c_str(), opt.port);

    // Watchdog: nudge the compositor when the screen is static so the sink
    // keeps receiving frames (and the very first frame appears at all).
    int64_t lastStats = nowUs();
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        int64_t t = nowUs();
        if (t - lastQueuedUs.load() > 500000)
            rec.requestRepaint();
        if (FILE *f = fopen("/tmp/imira-rotate", "r")) {
            int r = 0;
            if (fscanf(f, "%d", &r) == 1 && (r == 0 || r == 90 || r == 270))
                overrideRotation = r;
            fclose(f);
        } else {
            overrideRotation = -1; // no file = back to the sensor
        }
        // Live resolution: no file = Full HD, "720" = HD.
        int wantW = 1920, wantH = 1080, wantBr = 8000000;
        if (FILE *f = fopen("/tmp/imira-res", "r")) {
            int r = 0;
            if (fscanf(f, "%d", &r) == 1 && r == 720) {
                wantW = 1280;
                wantH = 720;
                wantBr = 5000000;
            }
            fclose(f);
        }
        if (FILE *f = fopen("/tmp/imira-audio-offset", "r")) {
            long ms = 0;
            if (fscanf(f, "%ld", &ms) == 1 && ms >= -2000 && ms <= 2000)
                audioOffsetUs = (long long)ms * 1000;
            fclose(f);
        }
        if (wantW != opt.width && pendingW.load() == 0) {
            pendingH = wantH;
            pendingBr = wantBr;
            pendingW = wantW; // last: acts as the "ready" flag
        }
        if (t - lastStats > 5000000) {
            fprintf(stderr, "imira-castd: %ld frames (%ld dropped)\n",
                    frames.load(), drops.load());
            lastStats = t;
        }
    }

    rec.stop();
    enc.stop();
    // Tears down the silence sink so parked media streams return to the
    // phone speaker the moment the cast ends.
    audio.stop();
    return 0;
}
