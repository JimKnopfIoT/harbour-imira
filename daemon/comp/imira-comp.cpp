/*
 * harbour-imira — imira-comp: the convergence compositor.
 *
 * A second, virtual screen for the TV. Runs its own Wayland socket
 * (imira-comp-0); apps launched onto it render here instead of lipstick —
 * the phone UI stays untouched. The scene (currently: every mapped surface
 * as a centered window on a dark desktop) is rendered offscreen at the cast
 * resolution via QQuickRenderControl, read back as RGBA and published in a
 * shared-memory frame buffer that imira-castd (--input shm) encodes and
 * streams. Milestone 1: windows show up; input & window management follow.
 *
 * Frame handoff protocol (/dev/shm/imira-comp-fb):
 *   header { u32 magic 'ICF1', u32 seq, u32 width, u32 height }
 *   followed by width*height*4 RGBA bytes.
 *   Writer: seq -> odd (writing), memcpy, seq -> even (done).
 *   Reader: copy while seq is even, re-check seq afterwards, else retry.
 */
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QTimer>

#include <QWaylandQuickCompositor>
#include <QWaylandQuickSurface>
#include <QWaylandSurfaceItem>
#include <QWaylandOutput>

namespace {

struct ShmHeader {
    quint32 magic;
    quint32 seq;
    quint32 width;
    quint32 height;
};
constexpr quint32 kMagic = 0x31464349; // "ICF1"
const char *kShmName = "/imira-comp-fb";

} // namespace

class Compositor : public QWaylandQuickCompositor
{
public:
    Compositor(QQuickWindow *window, int width, int height)
        : QWaylandQuickCompositor("imira-comp-0")
        , m_window(window)
        , m_width(width)
        , m_height(height)
    {
        addDefaultShell();
        createOutput(window, QStringLiteral("imira"),
                     QStringLiteral("virtual-tv"));
        primaryOutput()->setGeometry(QRect(0, 0, width, height));
    }

    void surfaceCreated(QWaylandSurface *surface) override
    {
        fprintf(stderr, "imira-comp: surface created (%s)\n",
                qPrintable(surface->className()));
        QObject::connect(surface, &QWaylandSurface::mapped, [this, surface]() {
            // Cover windows are the little home-screen cards — they belong
            // to the phone's launcher, not onto the TV desktop.
            const QString title = surface->title();
            const QString category = surface->windowProperties()
                                         .value(QStringLiteral("CATEGORY"))
                                         .toString();
            if (title.contains(QLatin1String("CoverWindow"))
                    || category == QLatin1String("cover")) {
                fprintf(stderr, "imira-comp: ignoring cover surface (%s)\n",
                        qPrintable(title));
                return;
            }
            QWaylandQuickSurface *qs =
                static_cast<QWaylandQuickSurface *>(surface);
            QWaylandSurfaceItem *item =
                static_cast<QWaylandSurfaceItem *>(createView(qs));
            item->setParentItem(m_window->contentItem());
            item->setTouchEventsEnabled(false);
            positionWindow(item);
            fprintf(stderr,
                    "imira-comp: surface mapped %dx%d (%s) yInverted=%d\n",
                    surface->size().width(), surface->size().height(),
                    qPrintable(title), item->isYInverted());
        });
        // A desktop-sized window, not a phone screen: ask the client to lay
        // itself out for the full output right away.
        surface->requestSize(QSize(m_width, m_height));
    }

private:
    void positionWindow(QWaylandSurfaceItem *item)
    {
        // Milestone 1 layout: center whatever the client gives us.
        const QSizeF s = QSizeF(item->surface()->size());
        item->setPosition(QPointF((m_width - s.width()) / 2.0,
                                  (m_height - s.height()) / 2.0));
    }

    QQuickWindow *m_window;
    int m_width;
    int m_height;
};

int main(int argc, char *argv[])
{
    // The GL context comes from lipstick's EGL via the wayland QPA — but we
    // never show a window (QQuickRenderControl renders offscreen), so
    // nothing appears on the phone. There is no usable "offscreen" platform
    // plugin on the device.
    setenv("QT_QPA_PLATFORM", "wayland", 0);
    QGuiApplication app(argc, argv);

    int width = 1280, height = 720, fps = 30;
    for (int i = 1; i < argc - 1; ++i) {
        if (strcmp(argv[i], "--width") == 0)
            width = atoi(argv[++i]);
        else if (strcmp(argv[i], "--height") == 0)
            height = atoi(argv[++i]);
        else if (strcmp(argv[i], "--fps") == 0)
            fps = atoi(argv[++i]);
    }

    // --- shared-memory frame buffer -------------------------------------
    const size_t frameBytes = (size_t)width * height * 4;
    shm_unlink(kShmName);
    int fd = shm_open(kShmName, O_CREAT | O_RDWR, 0644);
    if (fd < 0 || ftruncate(fd, sizeof(ShmHeader) + frameBytes) < 0) {
        fprintf(stderr, "imira-comp: shm setup failed\n");
        return 1;
    }
    auto *shm = static_cast<uint8_t *>(
        mmap(nullptr, sizeof(ShmHeader) + frameBytes,
             PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    auto *hdr = reinterpret_cast<ShmHeader *>(shm);
    uint8_t *frame = shm + sizeof(ShmHeader);
    hdr->magic = kMagic;
    hdr->seq = 0;
    hdr->width = width;
    hdr->height = height;

    // --- offscreen Qt Quick scene ---------------------------------------
    QSurfaceFormat format;
    format.setDepthBufferSize(16);
    format.setStencilBufferSize(8);

    QOpenGLContext context;
    context.setFormat(format);
    if (!context.create()) {
        fprintf(stderr, "imira-comp: no GL context\n");
        return 1;
    }

    QOffscreenSurface offscreen;
    offscreen.setFormat(context.format());
    offscreen.create();

    QQuickRenderControl renderControl;
    QQuickWindow window(&renderControl);
    window.setGeometry(0, 0, width, height);
    window.setColor(QColor(16, 24, 32)); // the "desktop" background

    context.makeCurrent(&offscreen);
    renderControl.initialize(&context);

    QOpenGLFramebufferObject fbo(
        width, height, QOpenGLFramebufferObject::CombinedDepthStencil);
    window.setRenderTarget(&fbo);

    // --- the compositor itself ------------------------------------------
    Compositor compositor(&window, width, height);
    Q_UNUSED(compositor);

    // --- render + publish loop ------------------------------------------
    QTimer frameTimer;
    frameTimer.setInterval(1000 / fps);
    QObject::connect(&frameTimer, &QTimer::timeout, [&]() {
        context.makeCurrent(&offscreen);
        renderControl.polishItems();
        renderControl.sync();
        renderControl.render();
        context.functions()->glFlush();

        fbo.bind();
        hdr->seq++;                     // odd: writing
        __sync_synchronize();
        context.functions()->glReadPixels(0, 0, width, height, GL_RGBA,
                                          GL_UNSIGNED_BYTE, frame);
        __sync_synchronize();
        hdr->seq++;                     // even: complete
        fbo.release();
    });
    frameTimer.start();

    // A killed compositor must not leave a stale frame buffer behind — a
    // reader latching onto it would stream one frozen frame forever.
    signal(SIGTERM, [](int) { QCoreApplication::quit(); });
    signal(SIGINT, [](int) { QCoreApplication::quit(); });

    fprintf(stderr, "imira-comp: %dx%d@%d on wayland socket imira-comp-0\n",
            width, height, fps);
    int rc = app.exec();
    shm_unlink(kShmName);
    return rc;
}
