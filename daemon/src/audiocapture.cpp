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
 */
#include "audiocapture.h"

#include <chrono>
#include <cstdio>
#include <cstring>

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
// How far the timeline may wander before it is re-anchored.
constexpr int64_t kDriftUs = 150000;
// Capture fragment handed over by PulseAudio. Asking for a tiny one (and
// with it a tight latency) pushed the null sink down to a ten millisecond
// deadline that a busy phone cannot meet — the music then broke up while
// it was played INTO the sink, before any of it reached us. Latency here
// costs nothing: it is subtracted from the timestamp, and the sink is a
// second away in any case.
constexpr size_t kFragBytes = kChunkBytes * 8;   // 80 ms

const char *kSilenceSink = "imira_cast";
const char *kSilenceMonitor = "imira_cast.monitor";
const char *kFallbackMonitor = "sink.deep_buffer.monitor";

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
    bool routing = false;                       // we own imira_cast + moves
    uint32_t silenceSink = PA_INVALID_INDEX;    // sink index of imira_cast
    uint32_t silenceModule = PA_INVALID_INDEX;  // its owner module (unload!)
    // The droid hardware sinks; only streams sitting on these are taken.
    // (Streams on e.g. a Bluetooth sink stay where the user put them.)
    uint32_t hwSinks[2] = { PA_INVALID_INDEX, PA_INVALID_INDEX };

    // Scratch for the sequential lookups in start(); each step waits on the
    // mainloop until its callback signals.
    bool stepDone = false;
    uint32_t foundSink = PA_INVALID_INDEX;
    uint32_t foundModule = PA_INVALID_INDEX;
    uint32_t loadedModule = PA_INVALID_INDEX;

    uint8_t chunk[kChunkBytes];
    size_t fill = 0;
    // PTS runs on the sample counter, anchored once at start and gently
    // re-anchored when drift vs the wall clock exceeds one chunk (rate
    // drift between the audio clock and CLOCK_MONOTONIC).
    int64_t anchor = 0;
    int64_t chunks = 0;
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
    void maybeSilence(const pa_sink_input_info *info);
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
    if (strcmp(i->name, "sink.primary_output") == 0)
        im->hwSinks[0] = i->index;
    else if (strcmp(i->name, "sink.deep_buffer") == 0)
        im->hwSinks[1] = i->index;
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
    if ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK)
            != PA_SUBSCRIPTION_EVENT_SINK_INPUT)
        return;
    const auto type = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
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

void AudioCapture::Impl::maybeSilence(const pa_sink_input_info *info)
{
    if (!routing || silenceSink == PA_INVALID_INDEX)
        return;
    if (info->sink == silenceSink)
        return;
    // Only grab streams playing on the phone hardware; anything the user
    // routed elsewhere (Bluetooth!) is none of our business.
    if (hwSinks[0] != PA_INVALID_INDEX || hwSinks[1] != PA_INVALID_INDEX) {
        if (info->sink != hwSinks[0] && info->sink != hwSinks[1])
            return;
    }
    const char *group = pa_proplist_gets(info->proplist, "policy.group");
    if (!groupGoesToTv(group))
        return;
    pa_operation *o = pa_context_move_sink_input_by_index(
        ctx, info->index, silenceSink, nullptr, nullptr);
    if (o)
        pa_operation_unref(o);
    const char *app =
        pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);
    fprintf(stderr, "imira-castd: routing stream #%u (%s) to the cast\n",
            info->index, app ? app : "?");
}

void AudioCapture::Impl::onRead(pa_stream *s)
{
    while (pa_stream_readable_size(s) > 0) {
        const void *data = nullptr;
        size_t nbytes = 0;
        if (pa_stream_peek(s, &data, &nbytes) < 0)
            return;
        if (!data) {            // hole in the stream
            if (nbytes) {
                holes++;
                holeBytes += nbytes;
                pa_stream_drop(s);
            }
            continue;
        }
        // Wall-clock time of this block's first sample. PulseAudio reports
        // one latency for the whole block, so every chunk inside it has to
        // be placed relative to the block's start — estimating the clock
        // again per chunk makes a large block look like a huge drift and
        // re-anchors the timeline several times per block.
        pa_usec_t blat = 0;
        int bneg = 0;
        if (pa_stream_get_latency(s, &blat, &bneg) < 0)
            blat = 0;
        const int64_t blockLatUs = bneg ? -(int64_t)blat : (int64_t)blat;
        const int64_t blockUs =
            (int64_t)nbytes * kChunkUs / (int64_t)kChunkBytes;
        const int64_t blockWall = nowUs() - blockLatUs - blockUs;

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
            // End of this chunk inside the block, minus its own length.
            const int64_t wall =
                blockWall
                + (int64_t)(nbytes - left) * kChunkUs / (int64_t)kChunkBytes
                - kChunkUs;
            const int64_t now = nowUs();
            if (anchor == 0)
                anchor = wall - chunks * kChunkUs;
            int64_t pts = anchor + chunks * kChunkUs;
            // Only genuine drift or a real gap should move the timeline.
            // The old window of two chunks (20 ms) was narrower than
            // ordinary scheduling jitter, so the anchor jumped several
            // times a second and the sink heard every jump.
            if (pts - wall > kDriftUs || wall - pts > kDriftUs) {
                anchor = wall - chunks * kChunkUs;
                pts = wall;
                reanchors++;
            }
            if (now - lastReport > 5000000) {
                if (holes != reportedHoles || reanchors != reportedReanchors
                        || dropouts != reportedDropouts)
                    fprintf(stderr, "imira-castd: audio %lld holes "
                                    "(%lld ms lost), %lld re-anchors, "
                                    "%lld silent gaps (%lld ms)\n",
                            (long long)holes,
                            (long long)(holeBytes / (kChunkBytes / 10)),
                            (long long)reanchors,
                            (long long)dropouts, (long long)dropoutMs);
                reportedHoles = holes;
                reportedReanchors = reanchors;
                reportedDropouts = dropouts;
                lastReport = now;
            }
            bool zero = true;
            for (size_t z = 0; z < kChunkBytes; z++) {
                if (chunk[z]) {
                    zero = false;
                    break;
                }
            }
            if (zero) {
                zeroRun++;
            } else {
                if (zeroRun >= 3) {      // 30 ms and more
                    dropouts++;
                    dropoutMs += zeroRun * kChunkUs / 1000;
                }
                zeroRun = 0;
            }
            chunks++;
            if (chunks == 1)
                fprintf(stderr, "imira-castd: audio flowing (pts=%lld)\n",
                        (long long)pts);
            cb(chunk, kChunkBytes, pts);
        }
        pa_stream_drop(s);
    }
}

bool AudioCapture::start(const std::string &source, const ChunkCallback &cb)
{

    if (m_impl)
        return true;

    // No explicit source = the default: build the silence sink, route media
    // into it, capture its monitor. An explicit IMIRA_AUDIO_SOURCE skips the
    // routing entirely and captures the named monitor (diagnostics).
    const bool wantRouting = source.empty();
    std::string src = wantRouting ? kSilenceMonitor : source;
    if (!isMonitorName(src.c_str())) {
        // Only sink monitors are acceptable capture devices, ever.
        fprintf(stderr, "imira-castd: refusing non-monitor audio source %s\n",
                src.c_str());
        return false;
    }

    Impl *im = new Impl;
    im->cb = cb;
    im->ml = pa_threaded_mainloop_new();
    pa_mainloop_api *api = pa_threaded_mainloop_get_api(im->ml);
    im->ctx = pa_context_new(api, "imira-castd");
    pa_context_set_state_callback(im->ctx, contextState, im->ml);

    auto fail = [&](const char *what) {
        fprintf(stderr, "imira-castd: audio capture failed (%s): %s\n", what,
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
            fprintf(stderr, "imira-castd: no silence sink, phone stays "
                            "audible\n");
            src = kFallbackMonitor;
        } else {
            im->silenceSink = im->foundSink;
            im->silenceModule = im->foundModule;
            im->routing = true;

            // 2. Which sinks are the phone hardware.
            im->stepDone = false;
            waitStep(im, pa_context_get_sink_info_list(im->ctx, listSinksCb,
                                                       im));

            // 3. From now on, grab every appearing media stream …
            pa_context_set_subscribe_callback(im->ctx, subscribeCb, im);
            im->stepDone = false;
            waitStep(im, pa_context_subscribe(
                             im->ctx, PA_SUBSCRIPTION_MASK_SINK_INPUT,
                             successStepCb, im));

            // 4. … and whatever is already playing right now.
            im->stepDone = false;
            waitStep(im, pa_context_get_sink_input_info_list(
                             im->ctx, sinkInputSweepCb, im));
        }
    }

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

    pa_buffer_attr attr;
    memset(&attr, 0xff, sizeof(attr));
    attr.fragsize = kFragBytes;

    // No ADJUST_LATENCY: it would make this stream's wish the sink's
    // deadline again (see kFragBytes).
    const pa_stream_flags_t flags = (pa_stream_flags_t)(
        PA_STREAM_DONT_MOVE |
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
        fprintf(stderr,
                "imira-castd: audio stream landed on %s, not a monitor — "
                "audio disabled\n", dev ? dev : "?");
        pa_threaded_mainloop_unlock(im->ml);
        stopImpl(im);
        return false;
    }
    im->verified = true;
    fprintf(stderr, "imira-castd: audio capture from %s%s\n", dev,
            im->routing ? " (local playback silenced)" : "");

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
        if (im->ctx && im->routing
                && im->silenceModule != PA_INVALID_INDEX) {
            // Unloading the null sink hands the parked streams back to the
            // real sink — the phone sounds normal again.
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
    stopImpl(m_impl);
    m_impl = nullptr;
}

} // namespace imira
