/*
 * imira::ScreenRecorder — standalone lipstick-recorder screen capture client
 * for Sailfish OS, pure wayland-client (no Qt/glib/boost), C++14.
 *
 * Modelled after CODeRUS/screencast (recorder part LGPL-2.1,
 * lipstick-recorder protocol by Giulio Camuffo / Jolla, giucam/nemomobile).
 *
 * Required runtime environment on the device (as in the screencast
 * reference, which runs as a systemd *user* unit "After=lipstick.service"):
 *   - Run as the session user that owns lipstick's Wayland socket
 *     (defaultuser/nemo, uid 100000). Root does NOT get the socket by
 *     default; a root process must set the variables below explicitly
 *     and be able to open the socket file.
 *   - XDG_RUNTIME_DIR=/run/user/100000
 *   - WAYLAND_DISPLAY as in the user session: "wayland-0" on older
 *     releases, "../../display/wayland-0" on newer ones (SFOS >= 3.4,
 *     compositor socket in /run/display).
 *   - The lipstick_recorder_manager global is only exported by lipstick,
 *     i.e. lipstick must be running on that display.
 *
 * Frame data contract (see FrameCallback below):
 *   - lipstick announces WL_SHM_FORMAT_ARGB8888 in the setup event, but the
 *     pixel bytes are a GL RGBA readback: memory order R,G,B,A per pixel
 *     (the screencast reference reads the buffer as QImage::Format_RGBA8888).
 *     The callback therefore reports DRM_FORMAT_ABGR8888 (little-endian
 *     packed) for that case; other wl_shm formats are passed through as
 *     their identical DRM fourcc values.
 *   - transform is the raw value from the frame event:
 *     1 = normal (origin top-left), 2 = y_inverted (rows bottom-up, the
 *     consumer must flip vertically).
 *   - width/height/stride come from the recorder setup event.
 */

#ifndef IMIRA_RECORDER_H
#define IMIRA_RECORDER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

struct wl_display;
struct wl_registry;
struct wl_shm;
struct wl_output;
struct wl_buffer;
struct wl_callback;
struct lipstick_recorder_manager;
struct lipstick_recorder;

namespace imira {

class ScreenRecorder
{
public:
    // Called once per captured frame from the recorder thread.
    // The pixel buffer is only valid for the duration of the call.
    using FrameCallback = std::function<void(const uint8_t *pixels,
                                             int width, int height, int stride,
                                             uint32_t drmFormat, int transform)>;

    ScreenRecorder();
    ~ScreenRecorder();

    ScreenRecorder(const ScreenRecorder &) = delete;
    ScreenRecorder &operator=(const ScreenRecorder &) = delete;

    // Connect to the Wayland display, bind globals, create the recorder and
    // start the event loop thread. Returns false if the display cannot be
    // reached or lipstick_recorder_manager / wl_shm / wl_output are missing.
    bool start(const FrameCallback &cb);

    // Stop the event loop thread and release all Wayland resources.
    // Safe to call multiple times; blocks until the thread has joined.
    void stop();

    // Ask the compositor to repaint as soon as possible. record_frame alone
    // only captures the *next* frame the compositor draws for other reasons;
    // call this to force a capture of a static screen (e.g. screenshots or
    // when no frame arrived for too long). No effect if no frame is pending.
    void requestRepaint();

    bool running() const { return m_running.load(); }

private:
    struct Buffer;

    // wl_registry listener
    static void handleGlobal(void *data, wl_registry *registry, uint32_t id,
                             const char *interface, uint32_t version);
    static void handleGlobalRemove(void *data, wl_registry *registry, uint32_t id);

    // lipstick_recorder listener
    static void handleSetup(void *data, lipstick_recorder *recorder,
                            int32_t width, int32_t height, int32_t stride,
                            int32_t format);
    static void handleFrame(void *data, lipstick_recorder *recorder,
                            wl_buffer *buffer, uint32_t time, int32_t transform);
    static void handleFailed(void *data, lipstick_recorder *recorder,
                             int32_t result, wl_buffer *buffer);
    static void handleCancelled(void *data, lipstick_recorder *recorder,
                                wl_buffer *buffer);

    void createBuffers();
    void destroyBuffers();
    void recordFrame();   // submit a free buffer via record_frame
    void eventLoop();     // thread body
    void teardown();      // release wayland resources (thread must be joined)

    FrameCallback m_callback;

    wl_display *m_display = nullptr;
    wl_registry *m_registry = nullptr;
    wl_shm *m_shm = nullptr;
    wl_output *m_output = nullptr;
    lipstick_recorder_manager *m_manager = nullptr;
    lipstick_recorder *m_recorder = nullptr;

    std::vector<Buffer *> m_buffers;

    // Geometry from the setup event.
    int m_width = 0;
    int m_height = 0;
    int m_stride = 0;
    uint32_t m_wlFormat = 0;
    bool m_setupDone = false;

    std::thread m_thread;
    std::atomic<bool> m_running { false };
    std::atomic<bool> m_failed { false };
    int m_wakeupPipe[2] = { -1, -1 };
    std::mutex m_mutex; // guards start/stop against each other
};

} // namespace imira

#endif // IMIRA_RECORDER_H
