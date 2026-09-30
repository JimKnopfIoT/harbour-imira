/*
 * harbour-imira — AAC-LC encoder via droidmedia.
 *
 * Measured on the J2 with a probe program: the encoder hands out
 * each access unit as soon as its 1024 samples are in, but the stream starts
 * with 2048 samples (42.7 ms) of priming silence, and the timestamps it
 * returns are unusable — nanoseconds instead of microseconds, rounded to
 * the input chunks. So the output PTS is computed here from the sample
 * count: access unit k carries input samples from k * 1024 - 2048 on.
 */
#include "aacencoder.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <droidmedia/droidmedia.h>
#include <droidmedia/droidmediacodec.h>

namespace imira {

namespace {

constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr int kFrameBytes = kChannels * 2;
constexpr int64_t kSamplesPerAu = 1024;
constexpr int64_t kPrimingSamples = 2048;

struct Chunk { uint8_t *data; };

void chunkRef(void *) {}

void chunkUnref(void *opaque) {
    Chunk *c = static_cast<Chunk *>(opaque);
    free(c->data);
    delete c;
}

void onData(void *opaque, DroidMediaCodecData *encoded) {
    if (encoded->codec_config)
        return;     // AudioSpecificConfig; ADTS carries the same facts
    static_cast<AacEncoder *>(opaque)->onAccessUnit(
        static_cast<const uint8_t *>(encoded->data.data), encoded->data.size);
}

void onError(void *, int err) {
    fprintf(stderr, "imira-castd: AAC encoder error %d\n", err);
}

void onEos(void *) {}

int onSizeChanged(void *, int32_t, int32_t) { return 0; }

} // namespace

bool AacEncoder::init(int bitrate, const OutputCallback &cb)
{
    if (!droid_media_init()) {
        m_error = "droid_media_init failed";
        return false;
    }
    m_out = cb;
    m_samplesIn = 0;
    m_ausOut = 0;
    m_stopping = false;
    m_anchors.clear();

    DroidMediaCodecEncoderMetaData meta;
    memset(&meta, 0, sizeof(meta));
    meta.parent.type = "audio/mp4a-latm";
    meta.parent.channels = kChannels;
    meta.parent.sample_rate = kRate;
    meta.bitrate = bitrate;
    // Capture chunks are 20 ms; leave room for bigger ones.
    meta.max_input_size = kRate / 10 * kFrameBytes;

    m_codec = droid_media_codec_create_encoder(&meta);
    if (!m_codec) {
        m_error = "no AAC encoder (droid_media_codec_create_encoder failed)";
        return false;
    }

    static DroidMediaCodecCallbacks ccb;
    ccb.signal_eos = onEos;
    ccb.error = onError;
    ccb.size_changed = onSizeChanged;
    droid_media_codec_set_callbacks(m_codec, &ccb, nullptr);

    static DroidMediaCodecDataCallbacks dcb;
    dcb.data_available = onData;
    droid_media_codec_set_data_callbacks(m_codec, &dcb, this);

    if (!droid_media_codec_start(m_codec)) {
        m_error = "droid_media_codec_start failed for AAC";
        droid_media_codec_destroy(m_codec);
        m_codec = nullptr;
        return false;
    }
    return true;
}

void AacEncoder::feed(const uint8_t *pcm, size_t size, int64_t ptsUs)
{
    if (!m_codec || size == 0)
        return;
    size -= size % kFrameBytes;
    // Larger than the input buffer the codec was set up with: split.
    const size_t maxBytes = kRate / 10 * kFrameBytes;
    while (size > 0) {
        const size_t n = size < maxBytes ? size : maxBytes;
        int64_t first;
        {
            std::lock_guard<std::mutex> l(m_lock);
            first = m_samplesIn;
            m_anchors.push_back({first, ptsUs});
            m_samplesIn += n / kFrameBytes;
        }
        Chunk *c = new Chunk{static_cast<uint8_t *>(malloc(n))};
        memcpy(c->data, pcm, n);
        DroidMediaCodecData data;
        memset(&data, 0, sizeof(data));
        data.data.data = c->data;
        data.data.size = n;
        // Only has to rise steadily; the output PTS comes from the anchors.
        data.ts = first * 1000000 / kRate;
        DroidMediaBufferCallbacks bcb;
        bcb.ref = chunkRef;
        bcb.unref = chunkUnref;
        bcb.data = c;
        droid_media_codec_queue(m_codec, &data, &bcb);
        pcm += n;
        size -= n;
        ptsUs += (int64_t)(n / kFrameBytes) * 1000000 / kRate;
    }
}

void AacEncoder::onAccessUnit(const uint8_t *au, size_t size)
{
    int64_t ptsUs;
    {
        std::lock_guard<std::mutex> l(m_lock);
        if (m_anchors.empty())
            return;
        const int64_t s = m_ausOut * kSamplesPerAu - kPrimingSamples;
        ++m_ausOut;
        // Drop anchors the stream has passed; keep the one that covers s.
        while (m_anchors.size() > 1 && m_anchors[1].sample <= s)
            m_anchors.pop_front();
        const Anchor &a = m_anchors.front();
        // For the priming units (s < 0) this runs back from the first
        // chunk: their silence plays right before the first real sample.
        ptsUs = a.ptsUs + (s - a.sample) * 1000000 / kRate;
    }
    if (m_out && !m_stopping)
        m_out(au, size, ptsUs);
}

void AacEncoder::stop()
{
    if (!m_codec)
        return;
    // Late output during the codec's shutdown goes nowhere.
    m_stopping = true;
    droid_media_codec_stop(m_codec);
    droid_media_codec_destroy(m_codec);
    m_codec = nullptr;
}

} // namespace imira
