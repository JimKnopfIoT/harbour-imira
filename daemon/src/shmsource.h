/*
 * harbour-imira — frame source for convergence mode.
 * Reads the RGBA frames imira-comp publishes in shared memory and feeds
 * them into the same callback the lipstick-recorder capture uses.
 */
#ifndef IMIRA_SHMSOURCE_H
#define IMIRA_SHMSOURCE_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace imira {

class ShmFrameSource {
public:
    // Same shape as ScreenRecorder's callback: pixels, width, height,
    // stride, drmFormat (unused, 0), transform (2 = y-inverted, GL readback
    // is bottom-up).
    using FrameCallback = std::function<void(const uint8_t *pixels, int width,
                                             int height, int stride,
                                             uint32_t drmFormat,
                                             int transform)>;

    bool start(int fps, const FrameCallback &cb);
    void stop();

private:
    std::atomic<bool> m_running{false};
    std::thread m_thread;
};

} // namespace imira

#endif
