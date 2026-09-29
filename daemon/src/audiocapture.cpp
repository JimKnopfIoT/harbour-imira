/*
 * harbour-imira — PulseAudio capture + local-silence routing.
 *
 * Uses the async pa_stream API (not pa_simple) for one reason: Sailfish's
 * module-policy-enforcement force-moves every record stream onto the active
 * input route (source.primary_input = the microphone), silently ignoring the
 * device the stream asked for. PA_STREAM_DONT_MOVE blocks that move, and it
 * is only settable through the full API. Belt and braces: after connecting we
 * verify the stream really sits on a *.monitor source and transmit nothing
 * otherwise — microphone audio must never leave the device.
 *
 * Local silence while casting: without help, the phone speaker plays along
 * with the TV (the sink keeps rendering what we monitor). So by default the
 * capture builds its own null sink "imira_cast", moves every media playback
 * stream there the moment it appears (subscription on sink-input events) and
 * records that sink's monitor. The phone stays silent, the TV gets the
 * audio, and the volume keys keep working because stream volumes apply
 * before the mix we tap. Ringtones, alarms and call audio are deliberately
 * left alone — those must keep sounding on the phone. On stop the null sink
 * is unloaded and PulseAudio hands the streams back to the real sink.
 *
 * Audio route (/tmp/imira-audio-route, switched live from the app):
 *   auto  media streams on the phone's own outputs go to the TV (default)
 *   all   every playback stream goes to the TV, Bluetooth and ringtones too
 *   <sink>.monitor  capture that output directly, no routing (the phone
 *         or the Bluetooth speaker keeps playing along)
 * Every decision lands in /tmp/imira-audio.log, which the diagnostic report
 * attaches: which outputs exist, which stream played where and why it was
 * or was not taken, and every 5 s whether the TV gets sound or silence.
 */
#include "audiocapture.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <ctime>
#include <map>

#include <pulse/pulseaudio.h>

namespace imira {

namespace {

int64_t nowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}


// 10 ms per chunk: 480 samples * 2 ch * 2 bytes.
constexpr size_t kChunkBytes = 480 * 2 * 2;
constexpr int64_t kChunkUs = 10000;
// Audio clock (details in onRead): sample position plus an offset taken from
// the minimum of arrival times. It is never moved in jumps once settled: the
// sink keys the picture to the audio clock, and the quarter-second jumps of
// 0.10.4 (re-anchor at 150 ms "drift" measured against PulseAudio's latency
// figure) made every frame late at once — picture stuttering or black, sound
// dropping out. Only a stall beyond kHardUs is followed at once.
constexpr int64_t kHardUs = 500000;
constexpr int64_t kMinWinUs = 2000000;      // window of the arrival minimum
constexpr int64_t kClockSettleUs = 10000000; // follow the minimum directly
constexpr int64_t kSlewUsPerS = 5000;       // then at most 5 ms per second
// Capture fragment, requested WITH ADJUST_LATENCY (see start()). PulseAudio
// gives the silencing sink the smallest latency any of its streams asks for.
// A player asks for about 100 ms, which makes a deadline of about 90 ms that
// a busy phone meets. This stream must ask for MORE, so that it never
// tightens that deadline — 10 ms (0.10.3) and even 80 ms made the music
// break up while it was played into the sink. But it must ask: without any
// request, i.e. with no player attached, PulseAudio falls back to about two
// seconds, and audio in two-second lumps turns the picture black.
constexpr size_t kFragBytes = kChunkBytes * 25;   // 250 ms

const char *kSilenceSink = "imira_cast";
const char *kSilenceMonitor = "imira_cast.monitor";
const char *kFallbackMonitor = "sink.deep_buffer.monitor";
const char *kRoutesPath = "/tmp/imira-audio-routes";

// Audio log: stderr (ends up in the handshake log) plus, stamped, the file
// the diagnostic report attaches. Level lines go to the file only — every
// 5 s would push the handshake out of the report's line limit.
FILE *audioLogFile()
{
    static FILE *f = [] {
        FILE *l = fopen("/tmp/imira-audio.log", "w");
        if (l)
            setvbuf(l, nullptr, _IOLBF, 0);
        return l;
    }();
    return f;
}

__attribute__((format(printf, 2, 3)))
void alog(bool toStderr, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (toStderr)
        fprintf(stderr, "imira-castd: %s\n", msg);
    if (FILE *f = audioLogFile()) {
        time_t t = time(nullptr);
        struct tm tmv;
        localtime_r(&t, &tmv);
        char line[560];
        snprintf(line, sizeof(line), "%02d:%02d:%02d %s\n",
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec, msg);
        fputs(line, f);
    }
}

const char *orQ(const char *s)
{
    return s && *s ? s : "?";
}

bool isMonitorName(const char *name)
{
    if (!name)
        return false;
    size_t n = strlen(name);
    return n > 8 && strcmp(name + n - 8, ".monitor") == 0;
}

// Policy groups whose streams belong on the TV. Everything else — ringtone,
// alarm, event, call, feedback… — keeps sounding locally on the phone.
bool groupGoesToTv(const char *group)
{
    if (!group)
        return true; // unclassified = plain media client
    static const char *kTvGroups[] = { "player", "game", "othermedia",
                                       "videoeditor", "flash", "alien" };
    for (const char *g : kTvGroups)
        if (strcmp(group, g) == 0)
            return true;
    return false;
}

} // namespace

struct AudioCapture::Impl {
    pa_threaded_mainloop *ml = nullptr;
    pa_context *ctx = nullptr;
    pa_stream *stream = nullptr;
    ChunkCallback cb;
    // Set once the connected device is confirmed to be a sink monitor;
    // until then (and forever if the check fails) no data is delivered.
    bool verified = false;

    // --- local-silence routing state ---
    enum Mode { Direct, Auto, All } mode = Auto;
    std::string route;                          // as asked for, for the log
    bool routing = false;                       // we own imira_cast + moves
    uint32_t silenceSink = PA_INVALID_INDEX;    // sink index of imira_cast
    uint32_t silenceModule = PA_INVALID_INDEX;  // its owner module (unload!)
    // Every sink, kept current by the subscription. "phone" = an output of
    // the droid card (the J2 has three: primary_output, deep_buffer, fast);
    // in auto mode only streams on those are taken — a stream on e.g. a
    // Bluetooth speaker stays where the user put it.
    struct Sink { std::string name; bool phone = false; };
    std::map<uint32_t, Sink> sinks;
    // Last logged decision per stream, so volume changes (CHANGE events)
    // do not repeat the same line.
    std::map<uint32_t, std::string> streamSeen;
    // Where each stream we took came from. Unloading the silence sink alone
    // does not bring them home: PulseAudio moves them to the default sink,
    // and on the J2 that is sink.null — the phone would stay mute after the
    // cast, and a direct capture after a route switch would hear nothing.
    std::map<uint32_t, uint32_t> origin;
    // Level report: loudest sample since the last one.
    int peak = 0;
    int64_t lastLevelUs = 0;

    // Scratch for the sequential lookups in start(); each step waits on the
    // mainloop until its callback signals.
    bool stepDone = false;
    uint32_t foundSink = PA_INVALID_INDEX;
    uint32_t foundModule = PA_INVALID_INDEX;
    uint32_t loadedModule = PA_INVALID_INDEX;

    uint8_t chunk[kChunkBytes];
    size_t fill = 0;
    // Chunks handed out (statistics; the PTS comes from the byte position).
    int64_t chunks = 0;
    // Arrival-minimum clock (see onRead).
    int64_t bytesRead = 0;
    int64_t base = 0;
    int64_t clockStartUs = 0;
    int64_t lastBlockUs = 0;
    std::deque<std::pair<int64_t, int64_t>> minWin;
    // Report: how far the offset lags its target, and how much was slewed.
    double driftEma = 0;
    int64_t slewedUs = 0;
    int64_t reportedSlewedUs = 0;
    // Diagnosis: samples actually read vs. the monotonic clock since the
    // first chunk, the last latency PulseAudio reported, and overflows
    // (PulseAudio silently discarding data we did not read in time).
    int64_t firstChunkUs = 0;
    int64_t lastLatUs = 0;
    int64_t overflows = 0;
    // Measurement session ("trace" in /tmp/imira-castd-flags): one CSV line
    // per block PulseAudio hands over, with its own timing info — the data
    // a robust audio clock gets designed from.
    FILE *trace = nullptr;
    long traceLines = 0;
    // Diagnosis for audible stutter: holes are samples PulseAudio could not
    // hand over (a real gap in the sound), re-anchors are jumps of the
    // timeline that the sink hears as a glitch. Reported together, rarely.
    int64_t holes = 0;
    int64_t holeBytes = 0;
    int64_t reanchors = 0;
    int64_t lastReport = 0;
    int64_t reportedHoles = 0;
    int64_t reportedReanchors = 0;
    // A null sink that is not fed in time renders silence rather than
    // reporting a hole, so a dropout in the played music reaches us as
    // digital zero. Music never is exactly zero, which makes a run of zero
    // chunks a reliable marker for audio that never arrived.
    int64_t zeroRun = 0;
    int64_t dropouts = 0;
    int64_t dropoutMs = 0;
    int64_t reportedDropouts = 0;

    void onRead(pa_stream *s);
    void traceBlock(pa_stream *s, size_t nbytes, bool hole);
    void maybeSilence(const pa_sink_input_info *info);
    void noteSink(const pa_sink_info *i, bool logIt);
    void publishRoutes();
    std::string sinkName(uint32_t idx) const;
};

namespace {

void contextState(pa_context *, void *ud)
{
    pa_threaded_mainloop_signal(static_cast<pa_threaded_mainloop *>(ud), 0);
}

void streamState(pa_stream *, void *ud)
{
    pa_threaded_mainloop_signal(static_cast<pa_threaded_mainloop *>(ud), 0);
}

void streamOverflow(pa_stream *, void *ud)
{
    static_cast<AudioCapture::Impl *>(ud)->overflows++;
}

void streamRead(pa_stream *s, size_t, void *ud)
{
    static_cast<AudioCapture::Impl *>(ud)->onRead(s);
}

// start()-sequence callbacks: every step ends by signalling the mainloop.

void findSinkCb(pa_context *, const pa_sink_info *i, int eol, void *ud)
{
    AudioCapture::Impl *im = static_cast<AudioCapture::Impl *>(ud);
    if (eol) {
        im->stepDone = true;
        pa_threaded_mainloop_signal(im->ml, 0);
        return;
    }
    im->foundSink = i->index;
    im->foundModule = i->owner_module;
}

void listSinksCb(pa_context *, const pa_sink_info *i, int eol, void *ud)
{
    AudioCapture::Impl *im = static_cast<AudioCapture::Impl *>(ud);
    if (eol) {
        im->stepDone = true;
        pa_threaded_mainloop_signal(im->ml, 0);
        return;
    }
    im->noteSink(i, true);
}

void loadModuleCb(pa_context *, uint32_t idx, void *ud)
{
    AudioCapture::Impl *im = static_cast<AudioCapture::Impl *>(ud);
    im->loadedModule = idx;
    im->stepDone = true;
    pa_threaded_mainloop_signal(im->ml, 0);
}

void successStepCb(pa_context *, int, void *ud)
{
    AudioCapture::Impl *im = static_cast<AudioCapture::Impl *>(ud);
    im->stepDone = true;
    pa_threaded_mainloop_signal(im->ml, 0);
}

// Event-path callbacks: these must NEVER signal the mainloop — they fire
// spontaneously and would wake unrelated waits.

void sinkEventCb(pa_context *, const pa_sink_info *i, int eol, void *ud)
{
    if (eol || !i)
        return;
    AudioCapture::Impl *im = static_cast<AudioCapture::Impl *>(ud);
    const bool known = im->sinks.count(i->index) != 0;
    im->noteSink(i, !known);
    if (!known)
        im->publishRoutes();
}

void sinkInputEventCb(pa_context *, const pa_sink_input_info *i, int eol,
                      void *ud)
{
    if (eol || !i)
        return;
    static_cast<AudioCapture::Impl *>(ud)->maybeSilence(i);
}

void sinkInputSweepCb(pa_context *, const pa_sink_input_info *i, int eol,
                      void *ud)
{
    AudioCapture::Impl *im = static_cast<AudioCapture::Impl *>(ud);
    if (eol) {
        im->stepDone = true;
        pa_threaded_mainloop_signal(im->ml, 0);
        return;
    }
    im->maybeSilence(i);
}

void subscribeCb(pa_context *c, pa_subscription_event_type_t t, uint32_t idx,
                 void *ud)
{
    AudioCapture::Impl *im = static_cast<AudioCapture::Impl *>(ud);
    const auto facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    const auto type = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
    if (facility == PA_SUBSCRIPTION_EVENT_SINK) {
        // Outputs come and go (a Bluetooth speaker connects): keep the list
        // the app cycles through current.
        if (type == PA_SUBSCRIPTION_EVENT_REMOVE) {
            auto it = im->sinks.find(idx);
            if (it != im->sinks.end()) {
                alog(true, "audio output gone: %s", it->second.name.c_str());
                im->sinks.erase(it);
                im->publishRoutes();
            }
            return;
        }
        pa_operation *o = pa_context_get_sink_info_by_index(c, idx,
                                                            sinkEventCb, im);
        if (o)
            pa_operation_unref(o);
        return;
    }
    if (facility != PA_SUBSCRIPTION_EVENT_SINK_INPUT)
        return;
    if (type == PA_SUBSCRIPTION_EVENT_REMOVE) {
        im->streamSeen.erase(idx);
        im->origin.erase(idx);
        return;
    }
    if (type != PA_SUBSCRIPTION_EVENT_NEW
            && type != PA_SUBSCRIPTION_EVENT_CHANGE)
        return;
    // A new or re-routed playback stream: look at it, maybe pull it over.
    pa_operation *o =
        pa_context_get_sink_input_info(c, idx, sinkInputEventCb, im);
    if (o)
        pa_operation_unref(o);
}

void waitStep(AudioCapture::Impl *im, pa_operation *o)
{
    if (!o) {
        im->stepDone = true;
        return;
    }
    while (!im->stepDone)
        pa_threaded_mainloop_wait(im->ml);
    pa_operation_unref(o);
}

} // namespace

std::string AudioCapture::Impl::sinkName(uint32_t idx) const
{
    auto it = sinks.find(idx);
    if (it != sinks.end())
        return it->second.name;
    return "sink #" + std::to_string(idx);
}

void AudioCapture::Impl::noteSink(const pa_sink_info *i, bool logIt)
{
    Sink &k = sinks[i->index];
    k.name = i->name ? i->name : "";
    k.phone = i->driver && strstr(i->driver, "droid");
    if (logIt && k.name != kSilenceSink)
        alog(true, "audio output %s (%s)", k.name.c_str(),
             k.phone ? "phone" : orQ(i->driver));
}

void AudioCapture::Impl::publishRoutes()
{
    // What the app's "Audio route" button cycles through: the two routing
    // modes, then every real output as a direct capture.
    const std::string tmp = std::string(kRoutesPath) + ".new";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f)
        return;
    fputs("auto\nall\n", f);
    for (const auto &k : sinks)
        if (k.second.name != kSilenceSink && !k.second.name.empty())
            fprintf(f, "%s.monitor\n", k.second.name.c_str());
    fclose(f);
    rename(tmp.c_str(), kRoutesPath);
}

void AudioCapture::Impl::maybeSilence(const pa_sink_input_info *info)
{
    const char *app =
        pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);
    const char *bin =
        pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_PROCESS_BINARY);
    const char *group = pa_proplist_gets(info->proplist, "policy.group");
    const char *role = pa_proplist_gets(info->proplist, PA_PROP_MEDIA_ROLE);
    // Never media.name: that is the song or video title.
    auto note = [&](const std::string &decision) {
        const std::string key = std::to_string(info->sink) + decision;
        auto it = streamSeen.find(info->index);
        if (it != streamSeen.end() && it->second == key)
            return;
        streamSeen[info->index] = key;
        alog(true, "stream #%u \"%s\" (%s, group %s, role %s) on %s: %s",
             info->index, orQ(app), orQ(bin), orQ(group), orQ(role),
             sinkName(info->sink).c_str(), decision.c_str());
    };

    if (routing && info->sink == silenceSink) {
        // Moved by us (already logged) or left over from an earlier run —
        // those go home to the media output when the cast ends.
        if (!streamSeen.count(info->index))
            note("already on the cast");
        if (!origin.count(info->index))
            for (const auto &k : sinks)
                if (k.second.name == "sink.deep_buffer")
                    origin[info->index] = k.first;
        streamSeen[info->index] = "cast";
        return;
    }
    if (!routing) {
        note("plays here (direct capture of " + route + ")");
        return;
    }
    auto sk = sinks.find(info->sink);
    const bool onPhone = sk != sinks.end() && sk->second.phone;
    if (mode == Auto && !onPhone) {
        note("stays (not a phone output; route \"all\" takes it)");
        return;
    }
    if (mode == Auto && !groupGoesToTv(group)) {
        note(std::string("stays (group ") + orQ(group)
             + " belongs to the phone; route \"all\" takes it)");
        return;
    }
    pa_operation *o = pa_context_move_sink_input_by_index(
        ctx, info->index, silenceSink, nullptr, nullptr);
    if (o)
        pa_operation_unref(o);
    origin[info->index] = info->sink;
    note("moved to the cast");
    streamSeen[info->index] = "cast";
}

void AudioCapture::Impl::traceBlock(pa_stream *s, size_t nbytes, bool hole)
{
    pa_usec_t lat = 0;
    int neg = 0;
    if (pa_stream_get_latency(s, &lat, &neg) < 0)
        lat = 0;
    timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    const pa_timing_info *ti = pa_stream_get_timing_info(s);
    fprintf(trace, "%lld,%lld,%lld,%zu,%d,%lld,%lld,%lld,%lld,%lld,%lld,%d\n",
            (long long)nowUs(),
            (long long)rt.tv_sec * 1000 + rt.tv_nsec / 1000000,
            (long long)(neg ? -(int64_t)lat : (int64_t)lat), nbytes, hole ? 1 : 0,
            (long long)chunks,
            ti ? (long long)ti->timestamp.tv_sec * 1000000 + ti->timestamp.tv_usec : 0LL,
            ti ? (long long)ti->source_usec : 0LL,
            ti ? (long long)ti->transport_usec : 0LL,
            ti ? (long long)ti->write_index : 0LL,
            ti ? (long long)ti->read_index : 0LL,
            ti ? ti->write_index_corrupt : -1);
    if (++traceLines % 20 == 0)
        fflush(trace);
}

void AudioCapture::Impl::onRead(pa_stream *s)
{
    while (pa_stream_readable_size(s) > 0) {
        const void *data = nullptr;
        size_t nbytes = 0;
        if (pa_stream_peek(s, &data, &nbytes) < 0)
            return;
        if (trace)
            traceBlock(s, nbytes, !data);
        if (!data) {            // hole in the stream
            if (nbytes) {
                holes++;
                holeBytes += nbytes;
                // Lost time still passed: keep the sample timeline aligned.
                bytesRead += nbytes;
                chunks += nbytes / kChunkBytes;
                pa_stream_drop(s);
            }
            continue;
        }
        // Audio clock. The timeline is the sample count plus an offset, and
        // the offset comes from ARRIVAL times, never from PulseAudio's
        // latency figure: measured on the J2, that figure wobbles around
        // zero (even negative) and reads 711 ms for the backlog of the first
        // blocks — the old anchor on the very first chunk put the whole
        // timeline 0.7 s off, sound and picture stayed away until a re-anchor
        // 45 s later. A block can never arrive before it was captured, so
        // the MINIMUM of (arrival - sample time of the block's end) over a
        // short window is a tight, robust bound; blocks delayed by a load
        // spike (turning the phone) only raise single values and are
        // ignored. The offset follows that minimum directly for the first
        // seconds, then by at most kSlewUsPerS — inaudible, and enough for
        // any real clock drift. Only a jump beyond kHardUs is followed at
        // once (the stream really stalled).
        {
            pa_usec_t blat = 0;
            int bneg = 0;
            if (pa_stream_get_latency(s, &blat, &bneg) == 0)
                lastLatUs = bneg ? -(int64_t)blat : (int64_t)blat;   // report only
        }
        const int64_t arrival = nowUs();
        bytesRead += nbytes;
        const int64_t endSampleUs = bytesRead * kChunkUs / (int64_t)kChunkBytes;
        const int64_t o = arrival - endSampleUs;
        while (!minWin.empty() && minWin.front().first < arrival - kMinWinUs)
            minWin.pop_front();
        while (!minWin.empty() && minWin.back().second >= o)
            minWin.pop_back();
        minWin.emplace_back(arrival, o);
        const int64_t target = minWin.front().second;
        if (clockStartUs == 0) {
            clockStartUs = arrival;
            base = target;
        } else if (arrival - clockStartUs < kClockSettleUs
                   || target - base > kHardUs || base - target > kHardUs) {
            if (arrival - clockStartUs >= kClockSettleUs)
                reanchors++;
            base = target;
        } else {
            const int64_t lim = (arrival - lastBlockUs) * kSlewUsPerS / 1000000 + 1;
            int64_t d = target - base;
            if (d > lim) d = lim;
            if (d < -lim) d = -lim;
            base += d;
            slewedUs += d > 0 ? d : -d;
        }
        lastBlockUs = arrival;
        driftEma = (double)(target - base);   // report: how far base lags

        const uint8_t *p = static_cast<const uint8_t *>(data);
        size_t left = nbytes;
        while (left > 0) {
            size_t take = kChunkBytes - fill;
            if (take > left)
                take = left;
            memcpy(chunk + fill, p, take);
            fill += take;
            p += take;
            left -= take;
            if (fill < kChunkBytes)
                continue;
            fill = 0;
            if (!verified)
                continue;       // never ship unverified audio
            const int64_t now = nowUs();
            // Start of this chunk by its byte position in the stream — not
            // by the chunk counter, which skips the chunks held back until
            // the source was verified.
            const int64_t pts = base
                + (bytesRead - (int64_t)left) * kChunkUs / (int64_t)kChunkBytes
                - kChunkUs;
            if (now - lastReport > 5000000) {
                if (holes != reportedHoles || reanchors != reportedReanchors
                        || dropouts != reportedDropouts
                        || slewedUs != reportedSlewedUs) {
                    const double el = firstChunkUs ? (double)(now - firstChunkUs) : 0;
                    fprintf(stderr, "imira-castd: audio clock: %.4f of real time "
                                    "(%lld ms read in %.1f s), latency %lld ms, "
                                    "%lld overflows\n",
                            el > 0 ? chunks * (double)kChunkUs / el : 0.0,
                            (long long)(chunks * kChunkUs / 1000), el / 1e6,
                            (long long)(lastLatUs / 1000), (long long)overflows);
                }
                if (holes != reportedHoles || reanchors != reportedReanchors
                        || dropouts != reportedDropouts
                        || slewedUs != reportedSlewedUs)
                    fprintf(stderr, "imira-castd: audio %lld holes "
                                    "(%lld ms lost), %lld re-anchors, "
                                    "slewed %lld ms, drift %lld ms, "
                                    "%lld silent gaps (%lld ms)\n",
                            (long long)holes,
                            (long long)(holeBytes / (kChunkBytes / 10)),
                            (long long)reanchors,
                            (long long)(slewedUs / 1000),
                            (long long)(driftEma / 1000),
                            (long long)dropouts, (long long)dropoutMs);
                reportedHoles = holes;
                reportedReanchors = reanchors;
                reportedDropouts = dropouts;
                reportedSlewedUs = slewedUs;
                lastReport = now;
            }
            int chunkPeak = 0;
            for (size_t z = 0; z + 1 < kChunkBytes; z += 2) {
                int v = (int16_t)(chunk[z] | (chunk[z + 1] << 8));
                if (v < 0)
                    v = -v;
                if (v > chunkPeak)
                    chunkPeak = v;
            }
            if (chunkPeak > peak)
                peak = chunkPeak;
            if (now - lastLevelUs > 5000000) {
                // What the TV hears: digital zero means nothing plays into
                // the captured output (e.g. the music plays elsewhere).
                if (lastLevelUs) {
                    if (peak == 0)
                        alog(false, "level: silence (digital zero)");
                    else
                        alog(false, "level: sound, peak %d dBFS",
                             (int)(20 * log10(peak / 32768.0)));
                }
                peak = 0;
                lastLevelUs = now;
            }
            const bool zero = chunkPeak == 0;
            if (zero) {
                zeroRun++;
            } else {
                if (zeroRun >= 3) {      // 30 ms and more
                    dropouts++;
                    dropoutMs += zeroRun * kChunkUs / 1000;
                }
                zeroRun = 0;
            }
            if (chunks == 0)
                firstChunkUs = now - kChunkUs;
            chunks++;
            if (chunks == 1)
                alog(true, "audio flowing (pts=%lld)", (long long)pts);
            cb(chunk, kChunkBytes, pts);
        }
        pa_stream_drop(s);
    }
}

bool AudioCapture::start(const std::string &source, const ChunkCallback &cb)
{

    if (m_impl)
        return true;

    // "" / "auto" / "all": build the silence sink, route streams into it,
    // capture its monitor. A monitor name skips the routing and captures
    // that output directly.
    Impl::Mode mode = Impl::Direct;
    if (source.empty() || source == "auto")
        mode = Impl::Auto;
    else if (source == "all")
        mode = Impl::All;
    const bool wantRouting = mode != Impl::Direct;
    std::string src = wantRouting ? kSilenceMonitor : source;
    if (!isMonitorName(src.c_str())) {
        // Only sink monitors are acceptable capture devices, ever.
        alog(true, "refusing non-monitor audio source %s", src.c_str());
        return false;
    }

    Impl *im = new Impl;
    im->mode = mode;
    im->route = source.empty() ? "auto" : source;
    if (mode == Impl::Auto)
        alog(true, "audio route auto: media on the phone's outputs goes to "
                   "the TV, the phone stays silent");
    else if (mode == Impl::All)
        alog(true, "audio route all: every playback stream goes to the TV");
    else
        alog(true, "audio route direct: capturing %s, no routing — it keeps "
                   "playing where it is", src.c_str());
    if (FILE *ff = fopen("/tmp/imira-castd-flags", "r")) {
        char w[64];
        while (fscanf(ff, "%63s", w) == 1)
            if (!strcmp(w, "trace")) {
                im->trace = fopen("/tmp/imira-audio-trace.csv", "w");
                if (im->trace)
                    fprintf(im->trace, "mono_us,real_ms,latency_us,nbytes,hole,"
                                       "chunks,ti_ts_us,source_usec,transport_usec,"
                                       "write_index,read_index,wi_corrupt\n");
            }
        fclose(ff);
    }
    im->cb = cb;
    im->ml = pa_threaded_mainloop_new();
    pa_mainloop_api *api = pa_threaded_mainloop_get_api(im->ml);
    im->ctx = pa_context_new(api, "imira-castd");
    pa_context_set_state_callback(im->ctx, contextState, im->ml);

    auto fail = [&](const char *what) {
        alog(true, "audio capture failed (%s): %s", what,
             pa_strerror(pa_context_errno(im->ctx)));
        pa_threaded_mainloop_unlock(im->ml);
        stopImpl(im);
        return false;
    };

    pa_threaded_mainloop_start(im->ml);
    pa_threaded_mainloop_lock(im->ml);

    if (pa_context_connect(im->ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0)
        return fail("context connect");
    for (;;) {
        pa_context_state_t st = pa_context_get_state(im->ctx);
        if (st == PA_CONTEXT_READY)
            break;
        if (!PA_CONTEXT_IS_GOOD(st))
            return fail("context");
        pa_threaded_mainloop_wait(im->ml);
    }

    if (wantRouting) {
        // 1. The silence sink: reuse a leftover instance or load a fresh one.
        im->stepDone = false;
        im->foundSink = PA_INVALID_INDEX;
        waitStep(im, pa_context_get_sink_info_by_name(im->ctx, kSilenceSink,
                                                      findSinkCb, im));
        if (im->foundSink == PA_INVALID_INDEX) {
            im->stepDone = false;
            im->loadedModule = PA_INVALID_INDEX;
            waitStep(im, pa_context_load_module(
                             im->ctx, "module-null-sink",
                             "sink_name=imira_cast rate=48000 "
                             "sink_properties=device.description=ImiraCast",
                             loadModuleCb, im));
            im->stepDone = false;
            im->foundSink = PA_INVALID_INDEX;
            waitStep(im, pa_context_get_sink_info_by_name(
                             im->ctx, kSilenceSink, findSinkCb, im));
        }
        if (im->foundSink == PA_INVALID_INDEX) {
            // No silence sink to be had — cast with audible phone rather
            // than with no audio at all.
            alog(true, "no silence sink, phone stays audible");
            src = kFallbackMonitor;
        } else {
            im->silenceSink = im->foundSink;
            im->silenceModule = im->foundModule;
            im->routing = true;
        }
    } else {
        // Direct capture: a silence sink left over from a crashed run would
        // still hold the streams, and the captured output would be silent.
        im->stepDone = false;
        im->foundSink = PA_INVALID_INDEX;
        waitStep(im, pa_context_get_sink_info_by_name(im->ctx, kSilenceSink,
                                                      findSinkCb, im));
        if (im->foundSink != PA_INVALID_INDEX
                && im->foundModule != PA_INVALID_INDEX) {
            im->stepDone = false;
            waitStep(im, pa_context_unload_module(
                             im->ctx, im->foundModule, successStepCb, im));
        }
    }

    // 2. Every output there is (which ones are the phone's own), for the
    //    routing decisions, the log and the app's route list.
    im->stepDone = false;
    waitStep(im, pa_context_get_sink_info_list(im->ctx, listSinksCb, im));
    im->publishRoutes();

    // 3. From now on, look at every appearing stream and output …
    pa_context_set_subscribe_callback(im->ctx, subscribeCb, im);
    im->stepDone = false;
    waitStep(im, pa_context_subscribe(
                     im->ctx, (pa_subscription_mask_t)(
                         PA_SUBSCRIPTION_MASK_SINK_INPUT
                         | PA_SUBSCRIPTION_MASK_SINK),
                     successStepCb, im));

    // 4. … and at whatever is already playing right now.
    im->stepDone = false;
    waitStep(im, pa_context_get_sink_input_info_list(
                     im->ctx, sinkInputSweepCb, im));

    pa_sample_spec spec;
    spec.format = PA_SAMPLE_S16LE;
    spec.rate = 48000;
    spec.channels = 2;

    // The xpolicy drop-in shipped as /etc/pulse/xpolicy.conf.d/imira.conf
    // matches application.name "imira-castd" (set at pa_context_new above)
    // into a nopolicy group — without it the policy would re-route this
    // stream onto the microphone at connect time, and the monitor check
    // below would disable audio.
    im->stream = pa_stream_new(im->ctx, "screen cast audio", &spec, nullptr);
    if (!im->stream)
        return fail("stream new");
    pa_stream_set_state_callback(im->stream, streamState, im->ml);
    pa_stream_set_read_callback(im->stream, streamRead, im);
    pa_stream_set_overflow_callback(im->stream, streamOverflow, im);

    pa_buffer_attr attr;
    memset(&attr, 0xff, sizeof(attr));
    attr.fragsize = kFragBytes;

    // ADJUST_LATENCY with the 250 ms fragment (see kFragBytes): bounded
    // latency when no player is attached, the player's deadline otherwise.
    const pa_stream_flags_t flags = (pa_stream_flags_t)(
        PA_STREAM_DONT_MOVE | PA_STREAM_ADJUST_LATENCY |
        PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE);
    if (pa_stream_connect_record(im->stream, src.c_str(), &attr, flags) < 0)
        return fail("stream connect");
    for (;;) {
        pa_stream_state_t st = pa_stream_get_state(im->stream);
        if (st == PA_STREAM_READY)
            break;
        if (!PA_STREAM_IS_GOOD(st))
            return fail("stream");
        pa_threaded_mainloop_wait(im->ml);
    }

    const char *dev = pa_stream_get_device_name(im->stream);
    if (!isMonitorName(dev)) {
        // The policy rerouted us anyway — bail out, silence over mic leak.
        alog(true, "audio stream landed on %s, not a monitor — audio "
                   "disabled", dev ? dev : "?");
        pa_threaded_mainloop_unlock(im->ml);
        stopImpl(im);
        return false;
    }
    im->verified = true;
    alog(true, "audio capture from %s", dev);

    pa_threaded_mainloop_unlock(im->ml);
    m_impl = im;
    return true;
}

void AudioCapture::stopImpl(Impl *im)
{
    if (!im)
        return;
    if (im->ml) {
        pa_threaded_mainloop_lock(im->ml);
        if (im->stream) {
            pa_stream_disconnect(im->stream);
            pa_stream_unref(im->stream);
        }
        const bool routed = im->routing;
        // Routing off before anything moves: each move home fires a CHANGE
        // event, and a live maybeSilence would pull the stream right back.
        im->routing = false;
        if (im->ctx)
            pa_context_set_subscribe_callback(im->ctx, nullptr, nullptr);
        if (im->ctx && routed) {
            // Streams home first (see origin), each to where it came from.
            for (const auto &o : im->origin) {
                if (!im->sinks.count(o.second))
                    continue;   // that output is gone (Bluetooth off)
                im->stepDone = false;
                waitStep(im, pa_context_move_sink_input_by_index(
                                 im->ctx, o.first, o.second, successStepCb,
                                 im));
            }
        }
        if (im->ctx && routed
                && im->silenceModule != PA_INVALID_INDEX) {
            // Unloading the null sink hands anything still parked to the
            // default sink.
            im->stepDone = false;
            waitStep(im, pa_context_unload_module(
                             im->ctx, im->silenceModule, successStepCb, im));
        }
        if (im->ctx) {
            pa_context_disconnect(im->ctx);
            pa_context_unref(im->ctx);
        }
        pa_threaded_mainloop_unlock(im->ml);
        pa_threaded_mainloop_stop(im->ml);
        pa_threaded_mainloop_free(im->ml);
    }
    delete im;
}

void AudioCapture::stop()
{
    if (m_impl)
        alog(true, "audio route %s stopped", m_impl->route.c_str());
    stopImpl(m_impl);
    m_impl = nullptr;
}

} // namespace imira
