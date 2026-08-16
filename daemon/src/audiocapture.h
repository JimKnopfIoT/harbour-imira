/*
 * harbour-imira — audio capture from the PulseAudio sink monitor.
 * Delivers the phone's audio output (what the speaker would play) as
 * S16LE 48 kHz stereo chunks with a monotonic-µs PTS.
 */
#ifndef IMIRA_AUDIOCAPTURE_H
#define IMIRA_AUDIOCAPTURE_H

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>

namespace imira {

class AudioCapture {
public:
    // Called on the capture thread; data is S16LE interleaved stereo,
    // valid only during the callback.
    using ChunkCallback = std::function<void(const uint8_t *pcm, size_t size,
                                             int64_t ptsUs)>;

    // source: PulseAudio source name; "" = the default, which builds the
    // "imira_cast" silence sink, routes media streams into it (phone goes
    // quiet, ringtones/calls stay local) and captures its monitor. An
    // explicit name captures that monitor without any routing. Only
    // *.monitor sources are accepted — a stream that ends up anywhere else
    // (Sailfish's routing policy tries to move record streams onto the
    // microphone) is torn down and start() returns false: no audio is ever
    // transmitted from a non-monitor source.
    bool start(const std::string &source, const ChunkCallback &cb);
    void stop();

    struct Impl;

private:
    static void stopImpl(Impl *im);
    Impl *m_impl = nullptr;
};

} // namespace imira

#endif
