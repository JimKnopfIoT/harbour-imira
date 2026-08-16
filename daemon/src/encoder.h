/*
 * harbour-imira — H.264 hardware encoder via droidmedia (Qualcomm OMX).
 * Feed raw I420/NV12 frames, receive encoded access units on a codec thread.
 */
#ifndef IMIRA_ENCODER_H
#define IMIRA_ENCODER_H

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>

typedef struct _DroidMediaCodec DroidMediaCodec;

namespace imira {

class H264Encoder {
public:
    // Called on the codec's thread. codecConfig=true carries SPS/PPS.
    using OutputCallback = std::function<void(const uint8_t *data, size_t size,
                                              int64_t ptsUs, bool idr,
                                              bool codecConfig)>;

    enum InputFormat { I420, NV12 };

    bool init(int width, int height, int fps, int bitrate,
              const OutputCallback &cb);
    // Tear the codec down and bring it back up: the first frame after a
    // restart is guaranteed to be an IDR with SPS/PPS (droidmedia has no
    // request-sync-frame API, this is the substitute).
    bool restart();
    // Restart with new dimensions/bitrate: the sink follows the new SPS in
    // the running stream, which makes live resolution switching possible.
    bool restartWith(int width, int height, int bitrate);
    // Takes ownership of frame (malloc'd); freed when the codec is done with it.
    bool queueFrame(uint8_t *frame, size_t size, int64_t ptsUs);
    void setBitrate(int bitrate);
    void stop();

    InputFormat inputFormat() const { return m_inputFormat; }
    const std::string &lastError() const { return m_error; }

private:
    DroidMediaCodec *m_codec = nullptr;
    OutputCallback m_out;
    InputFormat m_inputFormat = NV12;
    std::string m_error;
    int m_width = 0, m_height = 0, m_fps = 30, m_bitrate = 0;
};

} // namespace imira

#endif
