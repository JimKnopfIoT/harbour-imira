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
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <functional>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QProcess>

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QQuickItem>
#include <QQuickPaintedItem>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QSocketNotifier>
#include <QTimer>
#include <QVector>

#include <QWaylandClient>
#include <QWaylandInputDevice>
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

class Compositor;

// The dock: a bottom bar with launchable apps, rendered by the compositor
// itself (it is scene furniture, not a Wayland client). Clicking an icon
// starts the app — or, if its window already exists, restores and raises
// it: the dock doubles as the task bar.
class DockItem : public QQuickPaintedItem
{
public:
    static constexpr int kHeight = 48;

    struct App {
        QString id;
        QString name;
        QString exec;
        QImage icon;
        QRectF rect;
    };

    DockItem(QQuickItem *parent, int screenW, int screenH)
        : QQuickPaintedItem(parent)
    {
        setSize(QSizeF(screenW, kHeight));
        setPosition(QPointF(0, screenH - kHeight));
        setZ(9000);
        loadApps();
        // The selection page in the phone app writes the user config; pick
        // up changes live so the dock follows without a cast restart.
        auto *reload = new QTimer(this);
        QObject::connect(reload, &QTimer::timeout, [this]() {
            const QDateTime m =
                QFileInfo(userConfigPath()).lastModified();
            if (m != m_configStamp) {
                m_configStamp = m;
                m_apps.clear();
                loadApps();
                update();
            }
        });
        reload->start(3000);
        m_configStamp = QFileInfo(userConfigPath()).lastModified();
    }

    static QString userConfigPath()
    {
        return QDir::homePath()
               + QStringLiteral("/.config/imira/tv-apps");
    }

    void setCompositor(Compositor *comp) { m_comp = comp; }

    // Scene coordinates; true if the click was ours.
    bool handleClick(const QPointF &scenePos)
    {
        const QPointF p = scenePos - position();
        if (p.y() < 0)
            return false;
        for (const App &app : m_apps) {
            if (app.rect.contains(p)) {
                if (!activateExisting(app))
                    launch(app);
                return true;
            }
        }
        return true;            // clicks on empty dock stay in the dock
    }

    bool isRunning(const App &app) const;
    bool activateExisting(const App &app);

    void paint(QPainter *p) override
    {
        p->fillRect(boundingRect(), QColor(0, 0, 0, 180));
        p->setPen(QPen(QColor(255, 255, 255, 60), 1));
        p->drawLine(0, 0, width(), 0);
        for (const App &app : m_apps) {
            if (!app.icon.isNull()) {
                p->drawImage(app.rect.toRect(), app.icon);
            } else {
                p->setPen(Qt::white);
                p->drawRect(app.rect);
                p->drawText(app.rect, Qt::AlignCenter,
                            app.name.left(1).toUpper());
            }
            if (isRunning(app)) {   // the task-bar dot
                p->setPen(Qt::NoPen);
                p->setBrush(QColor(60, 200, 255));
                p->drawEllipse(QPointF(app.rect.center().x(),
                                       height() - 3), 2.5, 2.5);
            }
        }
    }

private:
    void loadApps()
    {
        // Which apps belong on the TV: /etc/imira/tv-apps (one .desktop
        // basename per line) or a sensible default set. Convergence is for
        // calendar, mail, notes, browsing — not for mirroring the settings.
        QStringList ids;
        QFile cfg(userConfigPath());
        if (!cfg.exists())
            cfg.setFileName(QStringLiteral("/etc/imira/tv-apps"));
        if (cfg.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!cfg.atEnd()) {
                const QString line =
                    QString::fromUtf8(cfg.readLine()).trimmed();
                if (!line.isEmpty() && !line.startsWith(QLatin1Char('#')))
                    ids << line;
            }
        }
        if (ids.isEmpty())
            ids << QStringLiteral("jolla-calendar")
                << QStringLiteral("jolla-notes")
                << QStringLiteral("jolla-email")
                << QStringLiteral("sailfish-office")
                << QStringLiteral("sailfish-browser")
                << QStringLiteral("jolla-gallery");

        qreal x = 12;
        for (const QString &id : ids) {
            QString name, iconName, exec;
            if (!parseDesktop(id, &name, &iconName, &exec))
                continue;
            App app;
            app.id = id;
            app.name = name;
            app.exec = exec;
            app.icon = loadIcon(iconName);
            app.rect = QRectF(x, (kHeight - 40) / 2.0, 40, 40);
            x += 40 + 12;
            m_apps.append(app);
        }
        fprintf(stderr, "imira-comp: dock with %d apps\n", m_apps.count());
    }

    bool parseDesktop(const QString &id, QString *name, QString *icon,
                      QString *exec)
    {
        QFile f(QStringLiteral("/usr/share/applications/") + id
                + QStringLiteral(".desktop"));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return false;
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (line.startsWith(QLatin1String("Name=")))
                *name = line.mid(5);
            else if (line.startsWith(QLatin1String("Icon=")))
                *icon = line.mid(5);
            else if (line.startsWith(QLatin1String("Exec=")) && exec->isEmpty())
                *exec = line.mid(5);
        }
        if (exec->isEmpty())
            return false;
        // Exec lines come as "app %u", "sailjail -p x /usr/bin/app" or
        // "invoker --type=y /usr/bin/app". We always want the plain binary:
        // sandboxing/boosting would wire the app to lipstick, not to us.
        const QStringList parts = exec->split(QLatin1Char(' '),
                                              QString::SkipEmptyParts);
        QString binary = parts.value(0);
        for (int i = parts.count() - 1; i > 0; --i) {
            if (parts.at(i).startsWith(QLatin1Char('/'))) {
                binary = parts.at(i);
                break;
            }
        }
        *exec = binary;
        return !binary.isEmpty() && !binary.contains(QLatin1String("invoker"))
               ? true
               : !binary.isEmpty();
    }

    QImage loadIcon(const QString &iconName)
    {
        if (iconName.startsWith(QLatin1Char('/')))
            return QImage(iconName);
        const QStringList sizes = { QStringLiteral("86x86"),
                                    QStringLiteral("128x128"),
                                    QStringLiteral("108x108"),
                                    QStringLiteral("172x172") };
        for (const QString &s : sizes) {
            const QString p = QStringLiteral("/usr/share/icons/hicolor/") + s
                              + QStringLiteral("/apps/") + iconName
                              + QStringLiteral(".png");
            QImage img(p);
            if (!img.isNull())
                return img;
        }
        // System apps keep their launcher icons inside the ambience theme.
        QDir theme(QStringLiteral("/usr/share/themes/sailfish-default/silica"));
        for (const QString &z : theme.entryList(
                 QStringList() << QStringLiteral("z*"), QDir::Dirs)) {
            QImage img(theme.filePath(z) + QStringLiteral("/icons/")
                       + iconName + QStringLiteral(".png"));
            if (!img.isNull())
                return img;
        }
        return QImage();
    }

    // One instance per app: Sailfish apps are single-instance by design;
    // a second direct start deadlocks on the first one's locks.
    bool alreadyRunning(const QString &exec) const
    {
        const QByteArray comm =
            exec.section(QLatin1Char('/'), -1).left(15).toUtf8();
        QDir proc(QStringLiteral("/proc"));
        for (const QString &pid : proc.entryList(QDir::Dirs)) {
            if (pid.at(0).isDigit()) {
                QFile f(QStringLiteral("/proc/") + pid
                        + QStringLiteral("/comm"));
                if (f.open(QIODevice::ReadOnly)
                        && f.readAll().trimmed() == comm)
                    return true;
            }
        }
        return false;
    }

    // Per-app launch environment for the TV. Some apps need desktop-mode
    // settings without touching their phone behaviour — fingerterm e.g.
    // draws its own on-screen keyboard unless its config says otherwise,
    // so it gets a separate config tree via XDG_CONFIG_HOME. Config:
    // ~/.config/imira/tv-app-env (fallback /etc/imira/tv-app-env), one
    // line per app: "<desktop-id> VAR=value [VAR=value …]".
    QString extraEnvFor(const QString &id) const
    {
        QFile f(QDir::homePath()
                + QStringLiteral("/.config/imira/tv-app-env"));
        if (!f.exists())
            f.setFileName(QStringLiteral("/etc/imira/tv-app-env"));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return QString();
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;
            const QStringList parts =
                line.split(QLatin1Char(' '), QString::SkipEmptyParts);
            if (parts.value(0) != id)
                continue;
            QStringList env;
            for (int i = 1; i < parts.count(); ++i)
                if (parts.at(i).contains(QLatin1Char('=')))
                    env << parts.at(i);
            return env.join(QLatin1Char(' '));
        }
        return QString();
    }

    void launch(const App &app)
    {
        if (alreadyRunning(app.exec)) {
            fprintf(stderr, "imira-comp: %s already running\n",
                    qPrintable(app.name));
            return;
        }
        fprintf(stderr, "imira-comp: launching %s (%s)\n",
                qPrintable(app.name), qPrintable(app.exec));
        // Env that a TV app needs; everything else is inherited from us.
        // QT_IM_MODULE=none: a desktop has a real keyboard — no Maliit
        // on-screen keyboard popping over the TV windows.
        QString cmd = QStringLiteral(
            "QT_QPA_PLATFORM=wayland WAYLAND_DISPLAY=imira-comp-0 "
            "QT_IM_MODULE=none "
            "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/100000/dbus/"
            "user_bus_socket ");
        const QString extra = extraEnvFor(app.id);
        if (!extra.isEmpty())
            cmd += extra + QLatin1Char(' ');
        cmd += QStringLiteral("exec ") + app.exec;
        QProcess::startDetached(QStringLiteral("/bin/sh"),
                                QStringList() << QStringLiteral("-c") << cmd);
    }

    QVector<App> m_apps;
    QDateTime m_configStamp;
    Compositor *m_comp = nullptr;

    friend class Compositor;
};

// A window on the TV desktop: title bar (drag to move, buttons to
// maximize/close) with the client surface below it.
class WindowChrome : public QQuickPaintedItem
{
public:
    static constexpr int kTitle = 32;

    WindowChrome(QQuickItem *parent, QWaylandSurfaceItem *item,
                 const QString &title)
        : QQuickPaintedItem(parent), m_item(item), m_title(title)
    {
        m_item->setParentItem(this);
        m_item->setPosition(QPointF(0, kTitle));
        syncToSurface();
    }

    void syncToSurface()
    {
        const QSize s = m_item->surface()->size();
        if (s.isEmpty())
            return;
        m_item->setSize(QSizeF(s));
        // Fixed-orientation apps (games!) draw rotated into their buffer
        // and announce it; the compositor has to undo that on screen.
        // Only compensate when the buffer SHAPE contradicts the announced
        // orientation: a fixed-orientation game draws sideways into a
        // portrait buffer (rotate!), while a Silica app in our landscape
        // window lays itself out correctly and merely reports its internal
        // orientation state (leave it alone — the 3DM file dialog came out
        // upside down when we rotated regardless). The heuristic still
        // guesses wrong for some dialogs, so the title bar has a rotate
        // button: rotOverride wins over the guess.
        int rot = rotOverride;
        if (rot < 0) {
            const bool portraitBuf = s.height() > s.width();
            switch (m_item->surface()->contentOrientation()) {
            case Qt::LandscapeOrientation:         rot = portraitBuf ? 90 : 0;  break;
            case Qt::InvertedPortraitOrientation:  rot = 180; break;
            case Qt::InvertedLandscapeOrientation: rot = portraitBuf ? 270 : 0; break;
            default:                               rot = 0;   break;
            }
        }
        m_item->setTransformOrigin(QQuickItem::Center);
        m_item->setRotation(rot);
        const bool swap = (rot == 90 || rot == 270);
        const qreal bw = swap ? s.height() : s.width();
        const qreal bh = swap ? s.width() : s.height();
        // The frame follows the user, not the client: userSize (set by
        // resize drag / maximize / clamping) fixes the frame, and the
        // (rotated) buffer is fitted into it aspect-preserving. Clients
        // that honour our size requests land at scale 1; clients with a
        // fixed surface (TilEm's calculator!) get scaled, never cropped.
        if (userSize.isEmpty()) {
            setSize(QSizeF(bw, bh + kTitle));
        } else if (zoomMode) {
            // Zoom mode: the frame snaps to the buffer's aspect ratio —
            // no letterbox bars, the content IS the window.
            const qreal s = qMin(userSize.width() / bw,
                                 (userSize.height() - kTitle) / bh);
            setSize(QSizeF(bw * s, bh * s + kTitle));
        } else {
            setSize(userSize);
        }
        const qreal cw = width();
        const qreal ch = height() - kTitle;
        const qreal scale = qMin(cw / bw, ch / bh);
        m_item->setScale(scale);
        // Scale and rotation pivot on the item's center: place the raw
        // buffer so its center sits at the content-area center.
        m_item->setPosition(QPointF((cw - s.width()) / 2.0,
                                    kTitle + (ch - s.height()) / 2.0));
        fprintf(stderr,
                "imira-comp: sync '%s' %dx%d orient=%d rot=%d scale=%.2f yInv=%d\n",
                qPrintable(m_item->surface()->title()), s.width(),
                s.height(), (int)m_item->surface()->contentOrientation(),
                rot, scale, m_item->isYInverted());
        update();
    }

    // One click = the window content turns 90° clockwise — the escape
    // hatch for windows where the orientation heuristic guesses wrong
    // (3DM viewer's file dialog). Four clicks come full circle.
    void cycleRotation()
    {
        rotOverride = ((rotOverride < 0 ? (int)m_item->rotation()
                                        : rotOverride) + 90) % 360;
        syncToSurface();
    }

    // Zoom mode: for apps that break when asked to relayout (games lay
    // out for the phone screen and crop; TilEm letterboxes internally —
    // double bars). On: send the app back to its phone-native geometry
    // once, then never ask again — the compositor only zooms, and the
    // frame follows the content's aspect. Off: ask it to relayout into
    // the current frame (fit mode, the default).
    void toggleZoom()
    {
        zoomMode = !zoomMode;
        if (zoomMode) {
            // Respect the app's orientation: a landscape-only game
            // (Machines vs. Machines) must not be forced into portrait.
            const QSize s = m_item->surface()->size();
            m_item->surface()->requestSize(s.width() > s.height()
                                               ? QSize(960, 540)
                                               : QSize(540, 960));
        } else
            m_item->surface()->requestSize(
                QSize((int)width(), (int)height() - kTitle));
        syncToSurface();
    }

    void setTitle(const QString &t) { m_title = t; update(); }

    QWaylandSurfaceItem *surfaceItem() const { return m_item; }

    // Hit areas in chrome-local coordinates.
    bool inTitle(const QPointF &p) const
    {
        return p.y() >= 0 && p.y() < kTitle;
    }
    bool inClose(const QPointF &p) const
    {
        return QRectF(width() - kTitle, 0, kTitle, kTitle).contains(p);
    }
    bool inMaximize(const QPointF &p) const
    {
        return QRectF(width() - 2 * kTitle, 0, kTitle, kTitle).contains(p);
    }
    bool inMinimize(const QPointF &p) const
    {
        return QRectF(width() - 3 * kTitle, 0, kTitle, kTitle).contains(p);
    }
    bool inRotate(const QPointF &p) const
    {
        return QRectF(width() - 4 * kTitle, 0, kTitle, kTitle).contains(p);
    }
    bool inZoom(const QPointF &p) const
    {
        return QRectF(width() - 5 * kTitle, 0, kTitle, kTitle).contains(p);
    }
    // Resizable from every non-title edge, like a real desktop — the old
    // corner-only grip was invisible on dark content (TilEm) and tiny on a
    // TV. Returns the edge mask under p; 0 = not a resize zone.
    enum ResizeEdge { EdgeLeft = 1, EdgeRight = 2, EdgeBottom = 4,
                      EdgeTop = 8 };
    int resizeEdgesAt(const QPointF &p) const
    {
        // Top-left corner of the title bar: diagonal resize (the right
        // corner belongs to the buttons).
        if (QRectF(0, 0, 26, kTitle).contains(p))
            return EdgeLeft | EdgeTop;
        if (p.y() < kTitle)
            return 0;           // rest of the title bar: move & buttons
        int e = 0;
        const qreal band = 12;
        if (p.x() < band)                 e |= EdgeLeft;
        if (p.x() > width() - band)       e |= EdgeRight;
        if (p.y() > height() - band)      e |= EdgeBottom;
        if (QRectF(width() - 26, height() - 26, 26, 26).contains(p))
            e |= EdgeRight | EdgeBottom;  // the classic corner grip
        return e;
    }

    bool minimized = false;
    bool maximized = false;
    QRectF restoreGeometry;
    QSizeF userSize;        // empty = the frame follows the buffer
    int rotOverride = -1;   // -1 = heuristic, else 0/90/180/270
    bool zoomMode = false;  // true = never ask the app to relayout

    void paint(QPainter *p) override
    {
        p->setRenderHint(QPainter::Antialiasing);
        // Opaque backdrop behind the client: Silica apps render translucent
        // (lipstick puts the ambience wallpaper behind them); without this
        // the windows shine through each other.
        QLinearGradient bg(0, kTitle, 0, height());
        bg.setColorAt(0.0, QColor(24, 34, 46));
        bg.setColorAt(1.0, QColor(10, 14, 20));
        p->fillRect(QRectF(0, kTitle, width(), height() - kTitle), bg);
        // Title bar
        p->fillRect(QRectF(0, 0, width(), kTitle), QColor(28, 40, 54, 255));
        p->setPen(QColor(255, 255, 255, 220));
        QFont f = p->font();
        f.setPixelSize(16);
        p->setFont(f);
        p->drawText(QRectF(32, 0, width() - 5 * kTitle - 36, kTitle),
                    Qt::AlignVCenter | Qt::AlignLeft, m_title);
        // Zoom toggle: a magnifier; filled when zoom mode is on
        const qreal zx = width() - 5 * kTitle;
        p->setPen(QPen(QColor(255, 255, 255, 200), 2));
        p->setBrush(zoomMode ? QBrush(QColor(255, 255, 255, 120))
                             : Qt::NoBrush);
        p->drawEllipse(QPointF(zx + 14, 14), 6, 6);
        p->drawLine(QPointF(zx + 18.5, 18.5),
                    QPointF(zx + kTitle - 8, kTitle - 8));
        // Top-left resize grip: diagonals mirroring the bottom-right one
        p->setPen(QPen(QColor(255, 255, 255, 220), 2));
        p->drawLine(QPointF(4, 20), QPointF(20, 4));
        p->drawLine(QPointF(4, 14), QPointF(14, 4));
        p->drawLine(QPointF(4, 8), QPointF(8, 4));
        // Rotate: an open circle (per-window rotation override)
        const qreal rx = width() - 4 * kTitle;
        p->setPen(QPen(QColor(255, 255, 255, 200), 2));
        p->setBrush(Qt::NoBrush);
        p->drawArc(QRectF(rx + 9, 9, kTitle - 18, kTitle - 18),
                   45 * 16, 270 * 16);
        // Minimize: a dash
        const qreal mnx = width() - 3 * kTitle;
        p->setPen(QPen(QColor(255, 255, 255, 200), 2));
        p->drawLine(QPointF(mnx + 10, kTitle - 12),
                    QPointF(mnx + kTitle - 10, kTitle - 12));
        // Maximize: a small square
        const qreal mx = width() - 2 * kTitle;
        p->setPen(QPen(QColor(255, 255, 255, 200), 2));
        p->setBrush(Qt::NoBrush);
        p->drawRect(QRectF(mx + 10, 10, kTitle - 20, kTitle - 20));
        // Close: an X
        const qreal cx = width() - kTitle;
        p->drawLine(QPointF(cx + 10, 10), QPointF(cx + kTitle - 10, kTitle - 10));
        p->drawLine(QPointF(cx + kTitle - 10, 10), QPointF(cx + 10, kTitle - 10));
        // Window border: makes the grabbable edges visible at TV distance.
        p->setPen(QPen(QColor(255, 255, 255, 110), 1));
        p->setBrush(Qt::NoBrush);
        p->drawRect(QRectF(0.5, 0.5, width() - 1, height() - 1));
        // Little pills at the edge centers: the resize zones, made visible.
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(255, 255, 255, 90));
        p->drawRoundedRect(QRectF(2, height() / 2 - 18, 4, 36), 2, 2);
        p->drawRoundedRect(
            QRectF(width() - 6, height() / 2 - 18, 4, 36), 2, 2);
        p->drawRoundedRect(
            QRectF(width() / 2 - 18, height() - 6, 36, 4), 2, 2);
        // Resize grip: three diagonals in the bottom-right corner
        p->setPen(QPen(QColor(255, 255, 255, 220), 2));
        p->drawLine(QPointF(width() - 20, height() - 4),
                    QPointF(width() - 4, height() - 20));
        p->drawLine(QPointF(width() - 14, height() - 4),
                    QPointF(width() - 4, height() - 14));
        p->drawLine(QPointF(width() - 8, height() - 4),
                    QPointF(width() - 4, height() - 8));
    }

private:
    QWaylandSurfaceItem *m_item;
    QString m_title;
};

// The TV cursor — drawn by us, the clients never see a system cursor.
class CursorItem : public QQuickPaintedItem
{
public:
    CursorItem(QQuickItem *parent) : QQuickPaintedItem(parent)
    {
        setSize(QSizeF(22, 30));
        setZ(10000);
    }
    void paint(QPainter *p) override
    {
        static const QPointF arrow[7] = {
            {0, 0}, {0, 24}, {6, 18}, {10, 28}, {14, 26}, {10, 16}, {18, 16},
        };
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(QPen(Qt::black, 1.5));
        p->setBrush(Qt::white);
        p->drawPolygon(arrow, 7);
    }
};

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
            item->setTouchEventsEnabled(false);
            auto *chrome = new WindowChrome(m_window->contentItem(), item,
                                            title.isEmpty()
                                                ? QStringLiteral("App")
                                                : title);
            // Cascade new windows instead of stacking them dead center.
            const int n = m_chromes.count();
            chrome->setPosition(QPointF(48 + (n % 8) * 36,
                                        24 + (n % 8) * 30));
            m_chromes.append(chrome);
            raise(chrome);
            clampChrome(chrome);
            if (m_dock)
                m_dock->update();
            QObject::connect(surface, &QWaylandSurface::sizeChanged,
                             [this, chrome]() {
                                 chrome->syncToSurface();
                                 clampChrome(chrome);
                             });
            QObject::connect(surface,
                             &QWaylandSurface::contentOrientationChanged,
                             [chrome]() { chrome->syncToSurface(); });
            QObject::connect(surface, &QWaylandSurface::titleChanged,
                             [chrome, surface]() {
                                 chrome->setTitle(surface->title());
                             });
            QObject::connect(surface, &QWaylandSurface::unmapped,
                             [this, chrome]() { removeChrome(chrome); });
            QObject::connect(surface, &QWaylandSurface::surfaceDestroyed,
                             [this, chrome]() { removeChrome(chrome); });
            // Belt and braces: whatever path tears the surface object down,
            // the chrome must never outlive it.
            QObject::connect(surface, &QObject::destroyed,
                             [this, chrome](QObject *) {
                                 removeChrome(chrome);
                             });
            fprintf(stderr,
                    "imira-comp: surface mapped %dx%d (%s) yInverted=%d\n",
                    surface->size().width(), surface->size().height(),
                    qPrintable(title), item->isYInverted());
        });
        // A desktop-sized window, not a phone screen: ask the client to lay
        // itself out for the workspace (output minus dock and title bar).
        surface->requestSize(QSize(m_width - 96,
                                   m_height - DockItem::kHeight
                                       - WindowChrome::kTitle - 48));
    }

    // Topmost window under the cursor (stacking order = list order).
    WindowChrome *chromeAt(const QPointF &pos) const
    {
        for (int i = m_chromes.count() - 1; i >= 0; --i) {
            WindowChrome *c = m_chromes.at(i);
            if (c->minimized)
                continue;
            const QRectF r(c->position(), QSizeF(c->width(), c->height()));
            if (r.contains(pos))
                return c;
        }
        return nullptr;
    }

    void setDock(DockItem *dock) { m_dock = dock; }

    // For the phone app's monitor: which windows are on the TV right now.
    // One line per window: "pid<TAB>title<TAB>minimized".
    void writeStatus() const
    {
        QFile f(QStringLiteral("/tmp/imira-tv-status.tmp"));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return;
        for (WindowChrome *c : m_chromes) {
            QWaylandSurface *surface = c->surfaceItem()->surface();
            f.write(QByteArray::number(
                        (qlonglong)surface->client()->processId()));
            f.write("\t");
            f.write(surface->title().toUtf8());
            f.write("\t");
            f.write(c->minimized ? "1" : "0");
            f.write("\n");
        }
        f.close();
        rename("/tmp/imira-tv-status.tmp", "/tmp/imira-tv-status");
    }

    void raise(WindowChrome *chrome)
    {
        m_chromes.removeAll(chrome);
        m_chromes.append(chrome);
        chrome->setZ(++m_zCounter);
        defaultInputDevice()->setKeyboardFocus(
            chrome->surfaceItem()->surface());
    }

    // Keep every window graspable: oversized clients are asked to shrink
    // to the workspace, and positions are clamped so title bar and resize
    // grip stay on screen.
    void clampChrome(WindowChrome *chrome)
    {
        const qreal wsW = m_width;
        const qreal wsH = m_height - DockItem::kHeight;
        if (chrome->width() > wsW || chrome->height() > wsH) {
            // Fix the frame to the workspace right away — a client that
            // ignores the shrink request gets scaled into it instead of
            // hanging over the screen edge.
            chrome->userSize = QSizeF(qMin(chrome->width(), wsW),
                                      qMin(chrome->height(), wsH));
            if (!chrome->zoomMode)
                chrome->surfaceItem()->surface()->requestSize(
                    QSize((int)qMin(chrome->width(), wsW),
                          (int)qMin(chrome->height(), wsH)
                              - WindowChrome::kTitle));
            chrome->syncToSurface();
        }
        QPointF p = chrome->position();
        p.setX(qBound(0.0, p.x(), qMax(0.0, wsW - chrome->width())));
        p.setY(qBound(0.0, p.y(), qMax(0.0, wsH - chrome->height())));
        chrome->setPosition(p);
    }

    void minimizeWindow(WindowChrome *chrome)
    {
        chrome->minimized = true;
        chrome->setVisible(false);
        if (m_dock)
            m_dock->update();
    }

    void restoreWindow(WindowChrome *chrome)
    {
        chrome->minimized = false;
        chrome->setVisible(true);
        raise(chrome);
        if (m_dock)
            m_dock->update();
    }

    // The window whose client binary matches the given comm name (15-byte
    // kernel-truncated), used by the dock's task-bar behaviour.
    WindowChrome *findByComm(const QByteArray &comm) const
    {
        for (WindowChrome *c : m_chromes) {
            const qint64 pid = c->surfaceItem()->surface()->client()
                                   ->processId();
            QFile f(QStringLiteral("/proc/%1/comm").arg(pid));
            if (f.open(QIODevice::ReadOnly)
                    && f.readAll().trimmed() == comm)
                return c;
        }
        return nullptr;
    }

    void toggleMaximize(WindowChrome *chrome)
    {
        QWaylandSurface *surface = chrome->surfaceItem()->surface();
        if (!chrome->maximized) {
            chrome->restoreGeometry =
                QRectF(chrome->position(),
                       QSizeF(chrome->width(), chrome->height()));
            chrome->setPosition(QPointF(0, 0));
            chrome->userSize =
                QSizeF(m_width, m_height - DockItem::kHeight);
            if (!chrome->zoomMode)
                surface->requestSize(QSize(
                    m_width,
                    m_height - DockItem::kHeight - WindowChrome::kTitle));
            chrome->maximized = true;
        } else {
            chrome->setPosition(chrome->restoreGeometry.topLeft());
            chrome->userSize = chrome->restoreGeometry.size();
            if (!chrome->zoomMode)
                surface->requestSize(QSize(
                    (int)chrome->restoreGeometry.width(),
                    (int)chrome->restoreGeometry.height()
                        - WindowChrome::kTitle));
            chrome->maximized = false;
        }
        chrome->syncToSurface();
    }

    void closeWindow(WindowChrome *chrome)
    {
        // wl_shell has no polite close request; SIGTERM is what the apps
        // handle cleanly anyway. If the client process is already gone
        // (zombie frame: the terminal died but its window stayed), there
        // is nobody left to exit for us — remove the chrome ourselves.
        const qint64 pid =
            chrome->surfaceItem()->surface()->client()->processId();
        if (!QFile::exists(QStringLiteral("/proc/%1").arg(pid))) {
            fprintf(stderr, "imira-comp: close on dead client %lld\n",
                    (long long)pid);
            removeChrome(chrome);
            return;
        }
        chrome->surfaceItem()->surface()->client()->kill(SIGTERM);
    }

    void removeChrome(WindowChrome *chrome)
    {
        if (!m_chromes.removeAll(chrome))
            return;
        fprintf(stderr, "imira-comp: window gone (%d left)\n",
                m_chromes.count());
        // Out of the scene NOW — the deferred delete alone left a ghost
        // frame on screen when a client died behind our back.
        chrome->setVisible(false);
        chrome->setParentItem(nullptr);
        chrome->deleteLater();
        if (!m_chromes.isEmpty())
            raise(m_chromes.last());
        if (m_dock)
            m_dock->update();
    }

private:

    QQuickWindow *m_window;
    int m_width;
    int m_height;
    int m_zCounter = 0;
    QVector<WindowChrome *> m_chromes;
    DockItem *m_dock = nullptr;
};

// Task-bar half of the dock (needs the full Compositor type).
bool DockItem::isRunning(const App &app) const
{
    if (!m_comp)
        return false;
    const QByteArray comm =
        app.exec.section(QLatin1Char('/'), -1).left(15).toUtf8();
    return m_comp->findByComm(comm) != nullptr;
}

bool DockItem::activateExisting(const App &app)
{
    if (!m_comp)
        return false;
    const QByteArray comm =
        app.exec.section(QLatin1Char('/'), -1).left(15).toUtf8();
    WindowChrome *chrome = m_comp->findByComm(comm);
    if (!chrome)
        return false;
    m_comp->restoreWindow(chrome);
    return true;
}

// Steals the external keyboard/pointer from lipstick (EVIOCGRAB) and feeds
// them into the compositor: the cursor lives on the TV, the phone's own
// touchscreen and buttons are untouched (they never match the capability
// checks below).
class InputManager : public QObject
{
public:
    InputManager(Compositor *comp, CursorItem *cursor, DockItem *dock,
                 int width, int height,
                 const std::function<void()> &screenshot)
        : m_comp(comp), m_cursor(cursor), m_dock(dock),
          m_screenshot(screenshot), m_w(width), m_h(height),
          m_pos(width / 2, height / 2)
    {
        m_cursor->setPosition(m_pos);
        scan();
        // Hotplug the cheap way: rescan for new devices every few seconds.
        auto *t = new QTimer(this);
        connect(t, &QTimer::timeout, this, [this]() { scan(); });
        t->start(3000);
    }

private:
    static bool hasBit(const unsigned long *bits, int bit)
    {
        return bits[bit / (8 * sizeof(long))]
               & (1ul << (bit % (8 * sizeof(long))));
    }

    void scan()
    {
        // A Bluetooth keyboard that reconnects gets NEW device nodes; the
        // old grabbed fds die silently. Drop them so the fresh nodes are
        // picked up below (the phone cursor came back as a zombie once).
        for (auto it = m_open.begin(); it != m_open.end();) {
            char probe[8];
            if (ioctl(it.value().fd, EVIOCGNAME(sizeof(probe)), probe) < 0) {
                fprintf(stderr, "imira-comp: input gone (%s)\n",
                        qPrintable(it.key()));
                // The notifier must die WITH the fd — a surviving notifier
                // would latch onto whatever new device reuses the number.
                it.value().notifier->setEnabled(false);
                it.value().notifier->deleteLater();
                close(it.value().fd);
                it = m_open.erase(it);
            } else {
                ++it;
            }
        }
        DIR *dir = opendir("/dev/input");
        if (!dir)
            return;
        while (dirent *e = readdir(dir)) {
            if (strncmp(e->d_name, "event", 5) != 0)
                continue;
            const QString path =
                QStringLiteral("/dev/input/") + QLatin1String(e->d_name);
            if (m_open.contains(path))
                continue;
            tryDevice(path);
        }
        closedir(dir);
    }

    void tryDevice(const QString &path)
    {
        int fd = open(path.toLocal8Bit().constData(),
                      O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            return;
        unsigned long keyBits[KEY_MAX / (8 * sizeof(long)) + 1] = {};
        unsigned long relBits[REL_MAX / (8 * sizeof(long)) + 1] = {};
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits);
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relBits)), relBits);
        const bool pointer = hasBit(relBits, REL_X) && hasBit(keyBits, BTN_LEFT);
        const bool keyboard =
            hasBit(keyBits, KEY_A) && hasBit(keyBits, KEY_ENTER)
            && !hasBit(relBits, REL_X);
        if (!pointer && !keyboard) {
            close(fd);
            return;             // touchscreen, phone buttons, sensors, …
        }
        char name[128] = "?";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        if (ioctl(fd, EVIOCGRAB, 1) < 0) {
            close(fd);
            return;
        }
        fprintf(stderr, "imira-comp: input %s: %s (%s)\n",
                pointer ? "pointer" : "keyboard", name,
                qPrintable(path));
        auto *notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        m_open.insert(path, { fd, notifier });
        connect(notifier, &QSocketNotifier::activated, this,
                [this, fd, pointer, path, notifier](int) {
                    if (!drain(fd, pointer)) {
                        notifier->setEnabled(false);
                        notifier->deleteLater();
                        close(fd);
                        m_open.remove(path);
                        fprintf(stderr, "imira-comp: input gone (%s)\n",
                                qPrintable(path));
                    }
                });
    }

    bool drain(int fd, bool pointer)
    {
        struct input_event ev;
        for (;;) {
            const ssize_t n = read(fd, &ev, sizeof(ev));
            if (n < 0)
                return errno == EAGAIN; // false = device disappeared
            if (n != sizeof(ev))
                return true;
            if (pointer)
                pointerEvent(ev);
            else
                keyEvent(ev);
        }
    }

    void pointerEvent(const input_event &ev)
    {
        QWaylandInputDevice *seat = m_comp->defaultInputDevice();
        if (ev.type == EV_REL && ev.code == REL_X) {
            m_pos.setX(qBound(0.0, m_pos.x() + ev.value, (double)m_w - 1));
        } else if (ev.type == EV_REL && ev.code == REL_Y) {
            m_pos.setY(qBound(0.0, m_pos.y() + ev.value, (double)m_h - 1));
        } else if (ev.type == EV_REL && ev.code == REL_WHEEL) {
            seat->sendMouseWheelEvent(Qt::Vertical, ev.value * 15);
            return;
        } else if (ev.type == EV_KEY
                   && (ev.code == BTN_LEFT || ev.code == BTN_RIGHT
                       || ev.code == BTN_MIDDLE)) {
            const Qt::MouseButton b = ev.code == BTN_LEFT ? Qt::LeftButton
                                    : ev.code == BTN_RIGHT ? Qt::RightButton
                                                           : Qt::MiddleButton;
            if (b == Qt::LeftButton && !ev.value && (m_drag || m_resize)) {
                m_drag = nullptr;   // end of a move…
                m_resize = nullptr; // …or of a resize
                return;
            }
            // The dock swallows its clicks before any window sees them.
            if (b == Qt::LeftButton && ev.value
                    && m_pos.y() >= m_dock->position().y()) {
                m_dock->handleClick(m_pos);
                return;
            }
            WindowChrome *chrome = m_comp->chromeAt(m_pos);
            if (!chrome)
                return;
            const QPointF inChrome = m_pos - chrome->position();
            const int edges = chrome->resizeEdgesAt(inChrome);
            if (b == Qt::LeftButton && ev.value && edges) {
                m_comp->raise(chrome);
                m_resize = chrome;
                m_resizeEdges = edges;
                m_resizeStart = m_pos;
                m_resizeGeo = QRectF(chrome->position(),
                                     QSizeF(chrome->width(),
                                            chrome->height()));
                chrome->maximized = false;
                return;
            }
            if (b == Qt::LeftButton && ev.value
                    && chrome->inTitle(inChrome)) {
                m_comp->raise(chrome);
                if (chrome->inClose(inChrome))
                    m_comp->closeWindow(chrome);
                else if (chrome->inMaximize(inChrome))
                    m_comp->toggleMaximize(chrome);
                else if (chrome->inMinimize(inChrome))
                    m_comp->minimizeWindow(chrome);
                else if (chrome->inRotate(inChrome))
                    chrome->cycleRotation();
                else if (chrome->inZoom(inChrome))
                    chrome->toggleZoom();
                else {          // grab the title bar: start moving
                    m_drag = chrome;
                    m_dragOffset = inChrome;
                }
                return;
            }
            // Click into the window content.
            QWaylandSurfaceItem *item = chrome->surfaceItem();
            const QPointF local = item->mapFromScene(m_pos);
            seat->sendMouseMoveEvent(item, local, m_pos);
            if (ev.value) {
                m_comp->raise(chrome);
                seat->sendMousePressEvent(b, local, m_pos);
            } else {
                seat->sendMouseReleaseEvent(b, local, m_pos);
            }
            return;
        } else if (ev.type == EV_SYN) {
            m_cursor->setPosition(m_pos);
            if (m_drag) {
                QPointF p = m_pos - m_dragOffset;
                p.setX(qBound(-(m_drag->width() - 120.0), p.x(),
                              (qreal)m_w - 120.0));
                p.setY(qBound(0.0, p.y(),
                              (qreal)m_h - DockItem::kHeight
                                  - WindowChrome::kTitle));
                m_drag->setPosition(p);
                return;
            }
            if (m_resize) {
                const QPointF d = m_pos - m_resizeStart;
                QRectF g = m_resizeGeo;
                if (m_resizeEdges & WindowChrome::EdgeRight)
                    g.setWidth(qMax(360.0, m_resizeGeo.width() + d.x()));
                if (m_resizeEdges & WindowChrome::EdgeBottom)
                    g.setHeight(qMax(280.0, m_resizeGeo.height() + d.y()));
                if (m_resizeEdges & WindowChrome::EdgeLeft) {
                    const qreal w =
                        qMax(360.0, m_resizeGeo.width() - d.x());
                    g = QRectF(m_resizeGeo.right() - w, g.y(),
                               w, g.height());
                }
                if (m_resizeEdges & WindowChrome::EdgeTop) {
                    const qreal h =
                        qMax(280.0, m_resizeGeo.height() - d.y());
                    g = QRectF(g.x(), m_resizeGeo.bottom() - h,
                               g.width(), h);
                }
                // The frame follows the mouse NOW; the client is asked to
                // relayout, and whatever it delivers is fitted into the
                // frame (syncToSurface) — no more cropping when it refuses.
                m_resize->setPosition(g.topLeft());
                m_resize->userSize = g.size();
                if (!m_resize->zoomMode)
                    m_resize->surfaceItem()->surface()->requestSize(
                        QSize((int)g.width(),
                              (int)g.height() - WindowChrome::kTitle));
                m_resize->syncToSurface();
                return;
            }
            WindowChrome *chrome = m_comp->chromeAt(m_pos);
            if (chrome && !chrome->inTitle(m_pos - chrome->position())) {
                QWaylandSurfaceItem *item = chrome->surfaceItem();
                seat->sendMouseMoveEvent(item, item->mapFromScene(m_pos),
                                         m_pos);
            }
        }
    }

    void keyEvent(const input_event &ev)
    {
        if (ev.type != EV_KEY || ev.value == 2) // no autorepeat doubling
            return;
        // Print key: pixel-perfect screenshot of the TV desktop.
        if ((ev.code == KEY_SYSRQ || ev.code == KEY_PRINT) && ev.value) {
            if (m_screenshot)
                m_screenshot();
            return;
        }
        QWaylandInputDevice *seat = m_comp->defaultInputDevice();
        // The API wants X-style keycodes (evdev + 8): without the offset,
        // pressing 'l' typed 'a' — exactly eight keys off.
        if (ev.value)
            seat->sendKeyPressEvent(ev.code + 8);
        else
            seat->sendKeyReleaseEvent(ev.code + 8);
    }

    Compositor *m_comp;
    CursorItem *m_cursor;
    DockItem *m_dock;
    std::function<void()> m_screenshot;
    int m_w, m_h;
    QPointF m_pos;
    WindowChrome *m_drag = nullptr;
    QPointF m_dragOffset;
    WindowChrome *m_resize = nullptr;
    int m_resizeEdges = 0;
    QPointF m_resizeStart;
    QRectF m_resizeGeo;
    struct OpenDevice {
        int fd;
        QSocketNotifier *notifier;
    };
    QHash<QString, OpenDevice> m_open;
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

    // Keyboard layout for the TV clients ("qwerty instead of qwertz"):
    // without an explicit keymap the clients fall back to US. Order:
    // ~/.config/imira/keymap (one word, e.g. "de"), IMIRA_KEYMAP env,
    // the country of $LANG, else US stays.
    {
        QString layout;
        QFile f(QDir::homePath() + QStringLiteral("/.config/imira/keymap"));
        if (f.open(QIODevice::ReadOnly | QIODevice::Text))
            layout = QString::fromUtf8(f.readLine()).trimmed();
        if (layout.isEmpty())
            layout = QString::fromLocal8Bit(qgetenv("IMIRA_KEYMAP"));
        const QString lang = QString::fromLocal8Bit(qgetenv("LANG"));
        if (layout.isEmpty() && lang.length() >= 5
                && lang.at(2) == QLatin1Char('_'))
            layout = lang.mid(3, 2).toLower();
        if (!layout.isEmpty()) {
            QWaylandKeymap keymap(layout);
            compositor.defaultInputDevice()->setKeymap(keymap);
            fprintf(stderr, "imira-comp: keymap '%s'\n",
                    qPrintable(layout));
        }
    }

    // --- dock, TV cursor, external keyboard/pointer ---------------------
    DockItem dock(window.contentItem(), width, height);
    dock.setCompositor(&compositor);
    compositor.setDock(&dock);
    CursorItem cursor(window.contentItem());
    InputManager input(&compositor, &cursor, &dock, width, height,
                       [&]() {
        // Print key: save the current frame, pixel-perfect.
        const QString dir = QDir::homePath()
                            + QStringLiteral("/Pictures/Screenshots");
        QDir().mkpath(dir);
        const QString path = dir + QStringLiteral("/TV_")
            + QDateTime::currentDateTime().toString(
                  QStringLiteral("yyyyMMdd_hhmmss"))
            + QStringLiteral(".png");
        QImage img(frame, width, height, width * 4,
                   QImage::Format_RGBA8888);
        img.mirrored(false, true).save(path);
        fprintf(stderr, "imira-comp: screenshot %s\n", qPrintable(path));
    });
    Q_UNUSED(input);

    // --- render + publish loop ------------------------------------------
    QTimer frameTimer;
    frameTimer.setInterval(1000 / fps);
    QObject::connect(&frameTimer, &QTimer::timeout, [&]() {
        context.makeCurrent(&offscreen);
        renderControl.polishItems();
        renderControl.sync();
        renderControl.render();
        context.functions()->glFlush();

        // Tell every client its frame was consumed — without this they
        // block on the wl_surface frame callback after a few frames (the
        // gallery froze exactly like that). Lipstick does the same after
        // each swap.
        compositor.sendFrameCallbacks(compositor.surfaces());

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

    // Status for the phone app's TV monitor, every 2 s.
    QTimer statusTimer;
    QObject::connect(&statusTimer, &QTimer::timeout,
                     [&compositor]() { compositor.writeStatus(); });
    statusTimer.start(2000);

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
