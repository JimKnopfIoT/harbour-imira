/*
  harbour-imira — CastController
  Copyright (C) 2026  harbour-imira contributors — GPLv3 or later.
*/
#include "castcontroller.h"

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

void CastController::poll()
{
    // Heartbeat: proves to the service that the app is still alive.
    touchFlag(kAlivePath);

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

    if (state != m_state || frames != m_frames || attempts != m_attempts
            || iface != m_iface || targetName != m_targetName
            || rotationMode != m_rotationMode || fullHd != m_fullHd
            || audioOffsetMs != m_audioOffsetMs) {
        m_state = state;
        m_frames = frames;
        m_attempts = attempts;
        m_iface = iface;
        m_targetName = targetName;
        m_rotationMode = rotationMode;
        m_fullHd = fullHd;
        m_audioOffsetMs = audioOffsetMs;
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
