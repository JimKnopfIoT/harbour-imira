/*
 * imira::ScreenRecorder implementation.
 * After CODeRUS/screencast (recorder part LGPL-2.1, giucam/nemomobile).
 * See recorder.h for the runtime environment and data contract.
 */

#include "recorder.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "lipstick-recorder-client-protocol.h"

namespace imira {

namespace {

// Number of shm buffers kept in flight. The screencast reference multi-
// buffers as well (default 48, sized for its network sender); a small ring
// is enough here since the callback consumes frames synchronously.
const int kBufferCount = 4;

// Local DRM fourcc constants to avoid a libdrm dependency.
constexpr uint32_t fourcc(char a, char b, char c, char d)
{
    return uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24);
}
constexpr uint32_t DRM_FORMAT_ABGR8888 = fourcc('A', 'B', '2', '4');
constexpr uint32_t DRM_FORMAT_XBGR8888 = fourcc('X', 'B', '2', '4');

// Map the wl_shm format announced in the setup event to the DRM fourcc of
// what the buffer actually contains. lipstick announces ARGB8888 but fills
// the buffer with a GL RGBA readback (bytes R,G,B,A per pixel), which is
// DRM_FORMAT_ABGR8888 in little-endian packing. All other wl_shm format
// values are already identical to their DRM fourcc.
uint32_t wlShmToDrmFormat(uint32_t wlFormat)
{
    switch (wlFormat) {
    case WL_SHM_FORMAT_ARGB8888: return DRM_FORMAT_ABGR8888;
    case WL_SHM_FORMAT_XRGB8888: return DRM_FORMAT_XBGR8888;
    default: return wlFormat;
    }
}

int createShmFile(size_t size)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir)
        dir = "/tmp";
    char path[256];
    snprintf(path, sizeof(path), "%s/imira-recorder-shm-XXXXXX", dir);

    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    unlink(path);

    int flags = fcntl(fd, F_GETFD);
    if (flags != -1)
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);

    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

} // namespace

struct ScreenRecorder::Buffer
{
    wl_buffer *buffer = nullptr;
    uint8_t *data = nullptr;
    size_t size = 0;
    bool busy = false;

    static Buffer *create(wl_shm *shm, int width, int height, int stride,
                          uint32_t wlFormat)
    {
        const size_t size = (size_t)stride * (size_t)height;
        int fd = createShmFile(size);
        if (fd < 0)
            return nullptr;

        void *data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            close(fd);
            return nullptr;
        }

        Buffer *buf = new Buffer;
        wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
        buf->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                                wlFormat);
        wl_buffer_set_user_data(buf->buffer, buf);
        wl_shm_pool_destroy(pool);
        close(fd);
        buf->data = static_cast<uint8_t *>(data);
        buf->size = size;
        return buf;
    }

    void destroy()
    {
        if (buffer)
            wl_buffer_destroy(buffer);
        if (data)
            munmap(data, size);
        delete this;
    }
};

ScreenRecorder::ScreenRecorder() = default;

ScreenRecorder::~ScreenRecorder()
{
    stop();
}

// ---- registry ----

void ScreenRecorder::handleGlobal(void *data, wl_registry *registry, uint32_t id,
                                  const char *interface, uint32_t version)
{
    ScreenRecorder *self = static_cast<ScreenRecorder *>(data);
    uint32_t v = version < 1u ? version : 1u;
    if (strcmp(interface, "lipstick_recorder_manager") == 0) {
        self->m_manager = static_cast<lipstick_recorder_manager *>(
            wl_registry_bind(registry, id, &lipstick_recorder_manager_interface, v));
    } else if (strcmp(interface, "wl_shm") == 0) {
        self->m_shm = static_cast<wl_shm *>(
            wl_registry_bind(registry, id, &wl_shm_interface, v));
    } else if (strcmp(interface, "wl_output") == 0 && !self->m_output) {
        // First output only; lipstick exposes a single output.
        self->m_output = static_cast<wl_output *>(
            wl_registry_bind(registry, id, &wl_output_interface, v));
    }
}

void ScreenRecorder::handleGlobalRemove(void *, wl_registry *, uint32_t)
{
}

// ---- lipstick_recorder events ----

void ScreenRecorder::handleSetup(void *data, lipstick_recorder *,
                                 int32_t width, int32_t height, int32_t stride,
                                 int32_t format)
{
    ScreenRecorder *self = static_cast<ScreenRecorder *>(data);

    // A repeated setup cancels pending frames and may change the geometry;
    // recreate the buffer ring in that case.
    self->destroyBuffers();

    self->m_width = width;
    self->m_height = height;
    self->m_stride = stride;
    self->m_wlFormat = (uint32_t)format;
    self->m_setupDone = true;

    self->createBuffers();
    self->recordFrame();
}

void ScreenRecorder::handleFrame(void *data, lipstick_recorder *,
                                 wl_buffer *buffer, uint32_t /*time*/,
                                 int32_t transform)
{
    ScreenRecorder *self = static_cast<ScreenRecorder *>(data);
    if (!self->m_running.load())
        return;

    // Queue the next capture first, as the reference does, so the
    // compositor can record its next frame while we process this one.
    self->recordFrame();

    Buffer *buf = static_cast<Buffer *>(wl_buffer_get_user_data(buffer));
    if (self->m_callback) {
        self->m_callback(buf->data, self->m_width, self->m_height,
                         self->m_stride, wlShmToDrmFormat(self->m_wlFormat),
                         (int)transform);
    }
    buf->busy = false;
}

void ScreenRecorder::handleFailed(void *data, lipstick_recorder *,
                                  int32_t result, wl_buffer *)
{
    ScreenRecorder *self = static_cast<ScreenRecorder *>(data);
    fprintf(stderr, "imira::ScreenRecorder: frame capture failed, result %d\n",
            (int)result);
    self->m_failed.store(true);
    self->m_running.store(false); // event loop exits after this dispatch
}

void ScreenRecorder::handleCancelled(void *, lipstick_recorder *,
                                     wl_buffer *buffer)
{
    // Our old buffer was replaced by a newer record_frame; reuse it.
    Buffer *buf = static_cast<Buffer *>(wl_buffer_get_user_data(buffer));
    buf->busy = false;
}

// ---- buffers ----

void ScreenRecorder::createBuffers()
{
    // lipstick's wl_shm only accepts the two canonical shm formats when
    // creating buffers, even though the recorder setup event may announce a
    // fourcc like RGBA8888. The GL readback fills the buffer with RGBA bytes
    // either way, so create as ARGB8888 and keep reporting the announced
    // format to the consumer.
    uint32_t bufFormat = (m_wlFormat == WL_SHM_FORMAT_ARGB8888 ||
                          m_wlFormat == WL_SHM_FORMAT_XRGB8888)
                             ? m_wlFormat
                             : WL_SHM_FORMAT_ARGB8888;
    for (int i = 0; i < kBufferCount; ++i) {
        Buffer *buf = Buffer::create(m_shm, m_width, m_height, m_stride,
                                     bufFormat);
        if (!buf) {
            fprintf(stderr, "imira::ScreenRecorder: shm buffer creation failed\n");
            m_failed.store(true);
            m_running.store(false);
            return;
        }
        m_buffers.push_back(buf);
    }
}

void ScreenRecorder::destroyBuffers()
{
    for (Buffer *buf : m_buffers)
        buf->destroy();
    m_buffers.clear();
}

void ScreenRecorder::recordFrame()
{
    Buffer *free = nullptr;
    for (Buffer *buf : m_buffers) {
        if (!buf->busy) {
            free = buf;
            break;
        }
    }
    if (!free) {
        // All buffers in flight; the next freed buffer will be used on the
        // following frame event.
        return;
    }
    free->busy = true;
    lipstick_recorder_record_frame(m_recorder, free->buffer);
    wl_display_flush(m_display);
}

// ---- lifecycle ----

bool ScreenRecorder::start(const FrameCallback &cb)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running.load() || m_display)
        return false;

    m_callback = cb;
    m_failed.store(false);
    m_setupDone = false;

    // Uses $WAYLAND_DISPLAY relative to $XDG_RUNTIME_DIR (see recorder.h).
    m_display = wl_display_connect(nullptr);
    if (!m_display) {
        fprintf(stderr, "imira::ScreenRecorder: cannot connect to Wayland display "
                        "(check WAYLAND_DISPLAY/XDG_RUNTIME_DIR and user)\n");
        return false;
    }

    m_registry = wl_display_get_registry(m_display);
    static const wl_registry_listener registryListener = {
        handleGlobal,
        handleGlobalRemove,
    };
    wl_registry_add_listener(m_registry, &registryListener, this);
    wl_display_roundtrip(m_display);

    if (!m_manager || !m_shm || !m_output) {
        fprintf(stderr, "imira::ScreenRecorder: missing globals "
                        "(lipstick_recorder_manager: %d, wl_shm: %d, wl_output: %d) "
                        "- is lipstick running on this display?\n",
                m_manager != nullptr, m_shm != nullptr, m_output != nullptr);
        teardown();
        return false;
    }

    m_recorder = lipstick_recorder_manager_create_recorder(m_manager, m_output);
    static const lipstick_recorder_listener recorderListener = {
        handleSetup,
        handleFrame,
        handleFailed,
        handleCancelled,
    };
    lipstick_recorder_add_listener(m_recorder, &recorderListener, this);

    // Running already so that setup/frame handlers behave normally; the
    // setup event creates the buffers and submits the first record_frame.
    m_running.store(true);
    wl_display_roundtrip(m_display);

    if (!m_setupDone || m_failed.load()) {
        fprintf(stderr, "imira::ScreenRecorder: recorder setup failed\n");
        m_running.store(false);
        teardown();
        return false;
    }

    if (pipe2(m_wakeupPipe, O_CLOEXEC | O_NONBLOCK) < 0) {
        m_running.store(false);
        teardown();
        return false;
    }

    m_thread = std::thread(&ScreenRecorder::eventLoop, this);
    return true;
}

void ScreenRecorder::stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_thread.joinable()) {
        m_running.store(false);
        // Wake the poll in the event loop.
        char c = 'q';
        ssize_t unused = write(m_wakeupPipe[1], &c, 1);
        (void)unused;
        m_thread.join();
    }
    teardown();
}

void ScreenRecorder::requestRepaint()
{
    // lipstick_recorder proxies are thread safe wrt. marshalling; flushing
    // from here is fine while the event loop thread only reads.
    if (m_recorder && m_running.load()) {
        lipstick_recorder_repaint(m_recorder);
        wl_display_flush(m_display);
    }
}

void ScreenRecorder::eventLoop()
{
    const int displayFd = wl_display_get_fd(m_display);

    while (m_running.load()) {
        // Standard prepare-read pattern so we can poll with a wakeup pipe.
        while (wl_display_prepare_read(m_display) != 0) {
            if (wl_display_dispatch_pending(m_display) < 0) {
                m_running.store(false);
                return;
            }
        }
        wl_display_flush(m_display);

        struct pollfd fds[2];
        fds[0].fd = displayFd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = m_wakeupPipe[0];
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        int ret = poll(fds, 2, -1);
        if (ret < 0 && errno != EINTR) {
            wl_display_cancel_read(m_display);
            m_running.store(false);
            break;
        }

        if (ret > 0 && (fds[0].revents & POLLIN)) {
            if (wl_display_read_events(m_display) < 0) {
                m_running.store(false);
                break;
            }
            if (wl_display_dispatch_pending(m_display) < 0) {
                m_running.store(false);
                break;
            }
        } else {
            wl_display_cancel_read(m_display);
        }

        if (ret > 0 && (fds[1].revents & POLLIN)) {
            char buf[16];
            while (read(m_wakeupPipe[0], buf, sizeof(buf)) > 0) {
            }
            break; // stop() requested
        }

        if (fds[0].revents & (POLLERR | POLLHUP)) {
            m_running.store(false);
            break;
        }
    }
}

void ScreenRecorder::teardown()
{
    destroyBuffers();

    if (m_recorder) {
        lipstick_recorder_destroy(m_recorder);
        m_recorder = nullptr;
    }
    if (m_manager) {
        lipstick_recorder_manager_destroy(m_manager);
        m_manager = nullptr;
    }
    if (m_output) {
        wl_output_destroy(m_output);
        m_output = nullptr;
    }
    if (m_shm) {
        wl_shm_destroy(m_shm);
        m_shm = nullptr;
    }
    if (m_registry) {
        wl_registry_destroy(m_registry);
        m_registry = nullptr;
    }
    if (m_display) {
        wl_display_flush(m_display);
        wl_display_disconnect(m_display);
        m_display = nullptr;
    }
    for (int i = 0; i < 2; ++i) {
        if (m_wakeupPipe[i] >= 0) {
            close(m_wakeupPipe[i]);
            m_wakeupPipe[i] = -1;
        }
    }
    m_setupDone = false;
    m_callback = nullptr;
}

} // namespace imira
