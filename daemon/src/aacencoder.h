/*
 * harbour-imira — AAC-LC encoder via droidmedia (Android's software codec,
 * c2.android.aac.encoder / OMX.google.aac.encoder). For sinks that offer
 * LPCM but only play AAC.
 */
#ifndef IMIRA_AACENCODER_H
#define IMIRA_AACENCODER_H

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

typedef struct _DroidMediaCodec DroidMediaCodec;

namespace imira {

class AacEncoder {
public:
    // Called on the codec's thread with one raw AAC-LC access unit (1024
    // samples, no ADTS header) and the presentation time of its first
    // sample in µs.
    using OutputCallback = std::function<void(const uint8_t *au, size_t size,
                                              int64_t ptsUs)>;

    // 48 kHz stereo only.
    bool init(int bitrate, const OutputCallback &cb);
    // S16LE interleaved stereo; ptsUs = time of the first sample. Chunks may
    // have any whole number of frames; a jump in ptsUs (capture restart,
    // clock re-anchor) carries through to the output timestamps.
    void feed(const uint8_t *pcm, size_t size, int64_t ptsUs);
    void stop();

    const std::string &lastError() const { return m_error; }

    // Only for the codec callback.
    void onAccessUnit(const uint8_t *au, size_t size);

private:
    // Input sample index where a chunk with a known PTS started.
    struct Anchor { int64_t sample; int64_t ptsUs; };

    DroidMediaCodec *m_codec = nullptr;
    OutputCallback m_out;
    std::string m_error;
    std::mutex m_lock;
    std::deque<Anchor> m_anchors;
    int64_t m_samplesIn = 0;
    int64_t m_ausOut = 0;
    std::atomic<bool> m_stopping{false};
};

} // namespace imira

#endif
