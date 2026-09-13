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
#include <vector>
#include <unistd.h>

#include "audiocapture.h"
#include "shmsource.h"
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

const char *stamp()
{
    static char buf[32];
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    tm tmv;
    localtime_r(&ts.tv_sec, &tmv);
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", tmv.tm_hour, tmv.tm_min,
             tmv.tm_sec, (int)(ts.tv_nsec / 1000000));
    return buf;
}

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
    std::atomic<int64_t> lastPatUs{0};

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
            std::unique_lock<std::mutex> l(sendLock);
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
            bool withPat = idr || (t - lastPatUs.load()) > 100000;
            if (withPat)
                lastPatUs = t;
            std::vector<uint8_t> ts;
            if (mux.packetize(data, size, ptsUs, idr, withPat, ts)) {
                if (dumpTs) {
                    fwrite(ts.data(), 1, ts.size(), dumpTs);
                    fflush(dumpTs);
                }
                // Send in slices and let go of the lock between them. When
                // the picture rate has collapsed — a still screen delivers
                // barely two frames a second — the encoder is allowed to
                // spend most of its per-second budget on the one frame it
                // does get, and a change of orientation then produces a
                // third of a megabyte at once. Sent as one block it pushes
                // every audio packet behind it, and that is what the sink
                // stutters on. Sliced, the sound keeps its turn.
                const size_t kSlice = 188 * 7 * 32;   // about 42 kB
                for (size_t off = 0; off < ts.size(); off += kSlice) {
                    const size_t n = ts.size() - off < kSlice ? ts.size() - off
                                                              : kSlice;
                    rtp.send(ts.data() + off, n, t);
                    if (off + n < ts.size()) {
                        l.unlock();
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(1));
                        l.lock();
                    }
                }
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
    // Next time a recorder frame is due (mirroring only; see onFrame).
    int64_t nextDueUs = 0;
    // When a frame last ARRIVED — not when one was last passed on. The
    // nudge below has to look at this: judging by the frames that got
    // through makes a discarded one look like a sleeping screen, so we
    // nudge again, and the extra frame is discarded in turn.
    std::atomic<int64_t> lastFrameUs{0};
    std::atomic<long> frames{0}, drops{0};
    // Convergence mode (IMIRA_INPUT=shm): frames come from imira-comp's
    // virtual TV screen instead of the lipstick recorder — the phone UI
    // stays interactive and is NOT mirrored.
    const char *inputEnv = getenv("IMIRA_INPUT");
    const bool shmInput = inputEnv && std::string(inputEnv) == "shm";

    // Content rotation (0/90/270): the orientation sensor keeps its own
    // value current; a value in /tmp/imira-rotate (polled below) overrides
    // it manually. Two separate atomics — the sensor must never fight the
    // override (it used to win for up to one poll interval per turn).
    // Rotation only exists to follow the phone while mirroring it. The
    // convergence screen is a landscape desktop of its own and must stay
    // put when the phone turns, so there the frame is never rotated —
    // neither by the sensor nor by the manual override.
    std::atomic<int> sensorRotation{0};
    std::atomic<int> overrideRotation{-1}; // -1 = automatic (sensor)
    if (!shmInput)
        startOrientationWatcher(&sensorRotation);
    // Live resolution switch via /tmp/imira-res ("720"/"1080"): applied on
    // the next captured frame through an encoder restart with new size.
    std::atomic<int> pendingW{0}, pendingH{0}, pendingBr{0};

    ScreenRecorder rec;
    ShmFrameSource shmSrc;
    auto onFrame = [&](const uint8_t *pixels, int width, int height,
                       int stride, uint32_t /*drmFormat*/, int transform) {
        int64_t t = nowUs();
        lastFrameUs = t;
        // Pace the recorder to the target rate by deadline, not by minimum
        // gap. The convergence compositor renders on a timer and needs no
        // pacing at all; the recorder fires on every damage event and does.
        // A minimum gap looks reasonable and behaves badly: a frame that
        // arrives a hair early is thrown away, and the next one is then a
        // whole period later, so the rate halves. Measured on a MediaTek
        // phone: twenty-seven frames a second arrived, fourteen went out.
        // A deadline that moves on by exactly one period keeps the average
        // right and discards only what is genuinely surplus.
        if (!shmInput) {
            if (t < nextDueUs) {
                drops++;
                return;
            }
            // After a pause (a still screen) the deadline would lag far
            // behind and let a burst through; restart it from now.
            nextDueUs = (t - nextDueUs > frameIntervalUs)
                            ? t + frameIntervalUs
                            : nextDueUs + frameIntervalUs;
        }
        // Encoder changes happen BEFORE the frame is built: the converter has
        // to be configured for the new size already, otherwise a frame of the
        // old geometry is queued into the freshly created encoder. Too large
        // overruns its input buffer — the MediaTek encoder dies right there
        // ("invalid handle" + error 4) and the stream never recovers.
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
        int ovr = overrideRotation.load();
        int rot = 0;
        if (!shmInput)
            rot = ovr >= 0 ? ovr : sensorRotation.load();
        size_t size = 0;
        uint8_t *frame = conv.convert(pixels, width, height, stride,
                                      transform == 2 /* y_inverted */, rot,
                                      &size);
        if (!frame)
            return;
        lastQueuedUs = t;
        frames++;
        // droidmedia takes microseconds in (MediaCodec convention) but
        // reports nanoseconds out — the output path divides by 1000.
        enc.queueFrame(frame, size, t);
    };
    ok = shmInput ? shmSrc.start(opt.fps, onFrame) : rec.start(onFrame);
    if (!ok) {
        fprintf(stderr, "imira-castd: cannot start %s\n",
                shmInput ? "comp frame source"
                         : "screen recorder "
                           "(WAYLAND_DISPLAY/XDG_RUNTIME_DIR correct?)");
        enc.stop();
        return 1;
    }

    fprintf(stderr, "imira-castd: streaming %dx%d@%d -> %s:%d\n", opt.width,
            opt.height, opt.fps, opt.dest.c_str(), opt.port);

    // The programme clock must not depend on the picture. It used to ride
    // along on video access units, which is fine at thirty frames a second
    // and hopeless with a still screen — the sink then saw the clock every
    // six hundred milliseconds instead of every hundred, lost its timing
    // and the audio broke up. This keeps it going regardless of what the
    // video path is doing.
    std::thread clockThread([&]() {
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            const int64_t t = nowUs();
            // Keep the picture moving at a steady rate. The recorder only
            // fires when something on screen changes, so a quiet screen used
            // to be nudged every 500 ms — two frames a second, with gaps of
            // three quarters of a second between presentation times. Sinks
            // that tie their audio output to the video clock stumble over
            // that, and it is heard as stuttering SOUND. A still picture
            // costs the encoder almost nothing, so the nudge is cheap.
            if (!shmInput && t - lastFrameUs.load() > frameIntervalUs)
                rec.requestRepaint();
            if (t - lastPatUs.load() <= 80000)
                continue;
            std::vector<uint8_t> ts;
            std::lock_guard<std::mutex> l(sendLock);
            if (t - lastPatUs.load() <= 80000)   // beaten to it by a frame
                continue;
            if (mux.packetizeClock(ts)) {
                lastPatUs = t;
                if (dumpTs) {
                    fwrite(ts.data(), 1, ts.size(), dumpTs);
                    fflush(dumpTs);
                }
                rtp.send(ts.data(), ts.size(), t);
            }
        }
    });

    // Watchdog: nudge the compositor when the screen is static so the sink
    // keeps receiving frames (and the very first frame appears at all).
    int64_t lastStats = nowUs();
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        int64_t t = nowUs();
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
            fprintf(stderr, "imira-castd: [%s] %ld frames (%ld dropped)\n",
                    stamp(), frames.load(), drops.load());
            lastStats = t;
        }
    }

    clockThread.join();
    if (shmInput)
        shmSrc.stop();
    else
        rec.stop();
    enc.stop();
    // Tears down the silence sink so parked media streams return to the
    // phone speaker the moment the cast ends.
    audio.stop();
    return 0;
}
