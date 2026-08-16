/*
  harbour-imira — CastController
  Copyright (C) 2026  harbour-imira contributors — GPLv3 or later.
*/
#include "castcontroller.h"

#include <algorithm>

#include <QDateTime>
#include <QDir>
#include <QImage>
#include <QFile>
#include <QStringList>

namespace {
const auto kStatusPath  = QStringLiteral("/tmp/imira-status");
const auto kStartFlag   = QStringLiteral("/tmp/imira-start");
const auto kStopFlag    = QStringLiteral("/tmp/imira-stop");
const auto kScanFlag    = QStringLiteral("/tmp/imira-scan");
const auto kDevicesPath = QStringLiteral("/tmp/imira-devices");
const auto kPeerPath    = QStringLiteral("/tmp/imira-peer");
const auto kTargetPath  = QStringLiteral("/tmp/imira-target");
const auto kAlivePath   = QStringLiteral("/tmp/imira-app-alive");
const auto kRotatePath  = QStringLiteral("/tmp/imira-rotate");
const auto kResPath     = QStringLiteral("/tmp/imira-res");
const auto kAudioOffPath = QStringLiteral("/tmp/imira-audio-offset");
const auto kModePath    = QStringLiteral("/tmp/imira-mode");

// Touch an empty flag file. Nothing to write — the file's existence is the
// message; the service removes it once acted upon.
void touchFlag(const QString &path)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.close();
}
} // namespace

CastController::CastController(QObject *parent)
    : QObject(parent)
{
    // 1 s matches the service's own update cadence; polling faster only
    // re-reads the same line.
    connect(&m_timer, &QTimer::timeout, this, &CastController::poll);
    m_timer.start(1000);
    poll();
}

CastController::~CastController()
{
    // App closed -> cast ends. The heartbeat file disappears with us, so the
    // service also notices if this destructor never runs (hard kill).
    if (m_state != QLatin1String("idle") && m_state != QLatin1String("nowlan"))
        stop();
    QFile::remove(kAlivePath);
}

void CastController::start()
{
    QFile::remove(kStopFlag);
    touchFlag(kStartFlag);
}

void CastController::stop()
{
    QFile::remove(kStartFlag);
    touchFlag(kStopFlag);
}

void CastController::scan()
{
    // The service answers with state "scanning" for the ~12 s the P2P find
    // takes, then rewrites /tmp/imira-devices; the poll picks both up.
    touchFlag(kScanFlag);
}

void CastController::selectDevice(const QString &mac)
{
    QFile f(kPeerPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(mac.toUtf8());
        f.write("\n");
    }
    // Reflect the choice immediately instead of waiting a poll tick — the
    // check mark should follow the tap, not the timer.
    if (m_selectedMac != mac) {
        m_selectedMac = mac;
        emit devicesChanged();
    }
}

void CastController::clearDevice()
{
    QFile::remove(kPeerPath);
    if (!m_selectedMac.isEmpty()) {
        m_selectedMac.clear();
        emit devicesChanged();
    }
}

void CastController::setRotationMode(const QString &mode)
{
    // "auto" removes the override file — the daemon's orientation sensor
    // takes over; "0"/"90" pin the rotation while the file exists.
    if (mode == QLatin1String("auto")) {
        QFile::remove(kRotatePath);
    } else {
        QFile f(kRotatePath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(mode.toUtf8());
            f.write("\n");
        }
    }
    if (m_rotationMode != mode) {
        m_rotationMode = mode;
        emit statusChanged();
    }
}

void CastController::setFullHd(bool on)
{
    // No file = Full HD default; "720" switches the next session to HD.
    if (on) {
        QFile::remove(kResPath);
    } else {
        QFile f(kResPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write("720\n");
    }
    if (m_fullHd != on) {
        m_fullHd = on;
        emit statusChanged();
    }
}

void CastController::setAudioOffset(int ms)
{
    // 0 removes the file — the daemon keeps its default of 0 ms; anything
    // else is picked up by the daemon's 250 ms poll, i.e. it applies live.
    if (ms == 0) {
        QFile::remove(kAudioOffPath);
    } else {
        QFile f(kAudioOffPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QByteArray::number(ms));
            f.write("\n");
        }
    }
    if (m_audioOffsetMs != ms) {
        m_audioOffsetMs = ms;
        emit statusChanged();
    }
}

void CastController::setConvergence(bool on)
{
    // No file = mirror mode; the service reads this at cast start.
    if (!on) {
        QFile::remove(kModePath);
    } else {
        QFile f(kModePath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write("convergence\n");
    }
    if (m_convergence != on) {
        m_convergence = on;
        emit statusChanged();
    }
}

namespace {
QString tvAppsPath()
{
    return QDir::homePath() + QStringLiteral("/.config/imira/tv-apps");
}
} // namespace

QString CastController::saveTvScreenshot() const
{
    QFile f(QStringLiteral("/dev/shm/imira-comp-fb"));
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    const QByteArray data = f.readAll();
    if (data.size() < 16)
        return QString();
    const quint32 *hdr = reinterpret_cast<const quint32 *>(data.constData());
    const quint32 w = hdr[2], h = hdr[3];
    if (hdr[0] != 0x31464349u || w == 0
            || (quint32)data.size() < 16 + w * h * 4)
        return QString();
    const QString dir = QDir::homePath()
                        + QStringLiteral("/Pictures/Screenshots");
    QDir().mkpath(dir);
    const QString name = QStringLiteral("TV_%1.png").arg(
        QDateTime::currentDateTime().toString(
            QStringLiteral("yyyyMMdd_hhmmss")));
    QImage img(reinterpret_cast<const uchar *>(data.constData()) + 16,
               w, h, w * 4, QImage::Format_RGBA8888);
    // The compositor's GL readback is bottom-up.
    if (!img.mirrored(false, true).save(dir + QLatin1Char('/') + name))
        return QString();
    return name;
}

QVariantList CastController::installedApps() const
{
    QVariantList result;
    const QDir dir(QStringLiteral("/usr/share/applications"));
    const QStringList files = dir.entryList(
        QStringList() << QStringLiteral("*.desktop"), QDir::Files);
    for (const QString &fn : files) {
        QFile f(dir.filePath(fn));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        QString name, icon;
        bool hidden = false;
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (line.startsWith(QLatin1String("Name=")) && name.isEmpty())
                name = line.mid(5);
            else if (line.startsWith(QLatin1String("Icon=")))
                icon = line.mid(5);
            else if (line == QLatin1String("NoDisplay=true")
                     || line == QLatin1String("Hidden=true"))
                hidden = true;
        }
        if (hidden || name.isEmpty() || icon.isEmpty())
            continue;
        QVariantMap app;
        app.insert(QStringLiteral("id"), fn.left(fn.length() - 8));
        app.insert(QStringLiteral("name"), name);
        app.insert(QStringLiteral("icon"), icon);
        result.append(app);
    }
    // Alphabetical by display name keeps the page scannable.
    std::sort(result.begin(), result.end(),
              [](const QVariant &a, const QVariant &b) {
                  return a.toMap().value(QStringLiteral("name")).toString()
                             .compare(b.toMap()
                                          .value(QStringLiteral("name"))
                                          .toString(),
                                      Qt::CaseInsensitive) < 0;
              });
    return result;
}

QStringList CastController::tvApps() const
{
    // The user's own selection, or the system default list as a starting
    // point (same fallback order the compositor uses).
    QFile f(tvAppsPath());
    if (!f.exists())
        f.setFileName(QStringLiteral("/etc/imira/tv-apps"));
    QStringList ids;
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (!line.isEmpty() && !line.startsWith(QLatin1Char('#')))
                ids << line;
        }
    }
    return ids;
}

void CastController::setTvApps(const QStringList &ids)
{
    QDir().mkpath(QDir::homePath() + QStringLiteral("/.config/imira"));
    QFile f(tvAppsPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    f.write("# Apps in der TV-Leiste (von der Imira-App verwaltet)\n");
    for (const QString &id : ids)
        f.write(id.toUtf8() + "\n");
}

namespace {
// utime+stime of a process in clock ticks, 0 if it is gone.
qulonglong jiffiesOf(qint64 pid)
{
    QFile f(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray stat = f.readAll();
    // Fields 14+15, counted after the parenthesized comm (which may
    // contain spaces).
    const int close = stat.lastIndexOf(')');
    const QList<QByteArray> fields = stat.mid(close + 2).split(' ');
    if (fields.count() < 13)
        return 0;
    return fields.at(11).toULongLong() + fields.at(12).toULongLong();
}

// PIDs whose comm matches (kernel-truncated names).
QList<qint64> pidsOf(const QByteArray &comm)
{
    QList<qint64> result;
    QDir proc(QStringLiteral("/proc"));
    for (const QString &entry : proc.entryList(QDir::Dirs)) {
        if (!entry.at(0).isDigit())
            continue;
        QFile f(QStringLiteral("/proc/") + entry + QStringLiteral("/comm"));
        if (f.open(QIODevice::ReadOnly) && f.readAll().trimmed() == comm)
            result << entry.toLongLong();
    }
    return result;
}
} // namespace

void CastController::updateTvMonitor()
{
    QStringList windows;
    QList<QPair<qint64, QString>> procs; // pid, display name
    QFile f(QStringLiteral("/tmp/imira-tv-status"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine());
            const QStringList parts = line.split(QLatin1Char('\t'));
            if (parts.count() < 3)
                continue;
            QString title = parts.at(1).trimmed();
            if (title.isEmpty())
                title = QStringLiteral("App");
            procs.append({ parts.at(0).toLongLong(), title });
            if (parts.at(2).trimmed() == QLatin1String("1"))
                title += QStringLiteral(" (min.)");
            windows << title;
        }
    }
    // The second instance's own machinery counts toward the load too.
    for (qint64 pid : pidsOf(QByteArrayLiteral("imira-comp")))
        procs.append({ pid, QStringLiteral("Compositor") });
    for (qint64 pid : pidsOf(QByteArrayLiteral("imira-castd")))
        procs.append({ pid, QStringLiteral("Encoder") });

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 dt = now - m_lastJiffiesMs;
    qulonglong deltaSum = 0;
    QHash<qint64, qulonglong> current;
    QVariantList list;
    for (const auto &proc : procs) {
        const qulonglong j = jiffiesOf(proc.first);
        if (j == 0)
            continue;
        current.insert(proc.first, j);
        int cpu = 0;
        if (m_lastJiffies.contains(proc.first) && m_lastJiffiesMs > 0
                && dt > 0) {
            const qulonglong delta = j - m_lastJiffies.value(proc.first);
            deltaSum += delta;
            // Ticks are 100/s: percent (all cores) over the poll interval.
            cpu = (int)(delta * 1000ull / (qulonglong)dt);
        }
        QVariantMap entry;
        entry.insert(QStringLiteral("name"), proc.second);
        entry.insert(QStringLiteral("cpu"), cpu);
        list.append(entry);
    }
    std::sort(list.begin(), list.end(),
              [](const QVariant &a, const QVariant &b) {
                  return a.toMap().value(QStringLiteral("cpu")).toInt()
                         > b.toMap().value(QStringLiteral("cpu")).toInt();
              });
    int load = 0;
    if (m_lastJiffiesMs > 0 && dt > 0)
        load = (int)(deltaSum * 1000ull / (qulonglong)dt);
    m_lastJiffies = current;
    m_lastJiffiesMs = now;

    if (windows != m_tvWindows || load != m_tvLoad || list != m_tvProcs) {
        m_tvWindows = windows;
        m_tvLoad = load;
        m_tvProcs = list;
        emit statusChanged();
    }
}

void CastController::poll()
{
    // Heartbeat: proves to the service that the app is still alive.
    touchFlag(kAlivePath);

    if (m_convergence)
        updateTvMonitor();

    QString state = QStringLiteral("idle");
    QString iface;
    int frames = 0;
    int attempts = 0;

    QFile f(kStatusPath);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // One line: "state frames attempts iface". iface may be absent while
        // no interface has been chosen yet.
        const QStringList parts = QString::fromUtf8(f.readLine())
                                      .simplified().split(QLatin1Char(' '));
        if (!parts.isEmpty() && !parts.at(0).isEmpty()) {
            state = parts.at(0);
            if (parts.size() > 1) frames   = parts.at(1).toInt();
            if (parts.size() > 2) attempts = parts.at(2).toInt();
            if (parts.size() > 3) iface    = parts.at(3);
        }
    }

    QString targetName;
    QFile tf(kTargetPath);
    if (tf.open(QIODevice::ReadOnly | QIODevice::Text))
        targetName = QString::fromUtf8(tf.readLine()).trimmed();

    QString rotationMode = QStringLiteral("auto");
    QFile rf(kRotatePath);
    if (rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString v = QString::fromUtf8(rf.readLine()).trimmed();
        if (v == QLatin1String("0") || v == QLatin1String("90")
                || v == QLatin1String("270"))
            rotationMode = v;
    }
    QFile resf(kResPath);
    const bool fullHd = !resf.exists();

    int audioOffsetMs = 0;
    QFile af(kAudioOffPath);
    if (af.open(QIODevice::ReadOnly | QIODevice::Text)) {
        bool ok = false;
        const int v = QString::fromUtf8(af.readLine()).trimmed().toInt(&ok);
        if (ok && v >= -2000 && v <= 2000)
            audioOffsetMs = v;
    }

    QFile mf(kModePath);
    bool convergence = false;
    if (mf.open(QIODevice::ReadOnly | QIODevice::Text))
        convergence = QString::fromUtf8(mf.readLine()).trimmed()
                      == QLatin1String("convergence");

    if (state != m_state || frames != m_frames || attempts != m_attempts
            || iface != m_iface || targetName != m_targetName
            || rotationMode != m_rotationMode || fullHd != m_fullHd
            || audioOffsetMs != m_audioOffsetMs
            || convergence != m_convergence) {
        m_state = state;
        m_frames = frames;
        m_attempts = attempts;
        m_iface = iface;
        m_targetName = targetName;
        m_rotationMode = rotationMode;
        m_fullHd = fullHd;
        m_audioOffsetMs = audioOffsetMs;
        m_convergence = convergence;
        emit statusChanged();
    }

    // --- device list: one peer per line, "mac<TAB>wfd<TAB>name" ------------
    QVariantList devices;
    QFile df(kDevicesPath);
    if (df.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!df.atEnd()) {
            const QString line = QString::fromUtf8(df.readLine());
            // The name is free text a sink announces about itself and may
            // itself contain tabs, so only the first two fields are split.
            const QString mac  = line.section(QLatin1Char('\t'), 0, 0).trimmed();
            const QString wfd  = line.section(QLatin1Char('\t'), 1, 1).trimmed();
            const QString name = line.section(QLatin1Char('\t'), 2).trimmed();
            if (mac.isEmpty())
                continue;
            // Only real Miracast sinks enter the model. Other P2P devices
            // (printers, laptops) stay in the scan file for diagnostics but
            // must not count as connectable receivers anywhere in the UI.
            if (wfd != QLatin1String("1"))
                continue;
            QVariantMap dev;
            dev.insert(QStringLiteral("mac"), mac);
            dev.insert(QStringLiteral("wfd"), wfd == QLatin1String("1"));
            dev.insert(QStringLiteral("name"), name);
            devices.append(dev);
        }
    }

    QString selectedMac;
    QFile pf(kPeerPath);
    if (pf.open(QIODevice::ReadOnly | QIODevice::Text))
        selectedMac = QString::fromUtf8(pf.readLine()).trimmed();

    const bool scanned = QFile::exists(kDevicesPath);
    if (devices != m_devices || selectedMac != m_selectedMac
            || scanned != m_scanned) {
        m_devices = devices;
        m_selectedMac = selectedMac;
        m_scanned = scanned;
        emit devicesChanged();
    }
}
