/*
  harbour-imira — CastController
  Copyright (C) 2026  harbour-imira contributors — GPLv3 or later.

  The whole UI<->service interface, seen from the app side. The system
  service (imira.service, running as root) owns Wi-Fi Direct, wpa_supplicant
  and the screen recorder; the app only

    - writes /tmp/imira-start or /tmp/imira-stop (empty flag files) and
    - polls /tmp/imira-status once a second.

  Status file format, one line: "state frames attempts iface", e.g.
      streaming 1843 2 wlan1
  state is one of idle/starting/scanning/connecting/streaming/error. Missing
  file or a malformed line reads as idle — the service simply is not running
  yet.

  Device discovery works the same way: /tmp/imira-scan (flag, written here)
  asks the service for a ~12 s P2P scan (state goes "scanning" meanwhile);
  the result lands in /tmp/imira-devices, one peer per line
      mac<TAB>wfd<TAB>name
  with wfd=1 for a real Miracast sink and 0 for any other P2P device (a
  printer, say). The chosen sink's MAC is written to /tmp/imira-peer;
  removing that file means "automatic" — the service takes the first sink
  it finds.
*/
#ifndef CASTCONTROLLER_H
#define CASTCONTROLLER_H

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

class CastController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state    READ state    NOTIFY statusChanged)
    Q_PROPERTY(int     frames   READ frames   NOTIFY statusChanged)
    Q_PROPERTY(int     attempts READ attempts NOTIFY statusChanged)
    Q_PROPERTY(QString iface    READ iface    NOTIFY statusChanged)
    // Entries are maps {mac, name, wfd:bool}, in the service's order.
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    // Empty string = automatic (first sink found).
    Q_PROPERTY(QString selectedMac READ selectedMac NOTIFY devicesChanged)
    // True once a scan has produced a result file (even an empty one).
    Q_PROPERTY(bool scanned READ scanned NOTIFY devicesChanged)
    // Name of the sink the service is currently talking to ("" = none yet).
    Q_PROPERTY(QString targetName READ targetName NOTIFY statusChanged)
    // "auto" (orientation sensor), "0" (portrait) or "90" (landscape).
    Q_PROPERTY(QString rotationMode READ rotationMode NOTIFY statusChanged)
    // true = Full HD (1080p), false = HD (720p); applies to the NEXT session.
    Q_PROPERTY(bool fullHd READ fullHd NOTIFY statusChanged)
    // A/V lip-sync trim in ms, positive = audio later. Applies live.
    Q_PROPERTY(int audioOffsetMs READ audioOffsetMs NOTIFY statusChanged)

public:
    explicit CastController(QObject *parent = nullptr);
    // Closing the app ends the cast (user decision): the destructor stops a
    // running session; a 1 s heartbeat file covers hard kills.
    ~CastController() override;

    QString state() const    { return m_state; }
    int     frames() const   { return m_frames; }
    int     attempts() const { return m_attempts; }
    QString iface() const    { return m_iface; }
    QVariantList devices() const { return m_devices; }
    QString selectedMac() const  { return m_selectedMac; }
    bool    scanned() const      { return m_scanned; }
    QString targetName() const   { return m_targetName; }

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void scan();
    Q_INVOKABLE void selectDevice(const QString &mac);
    Q_INVOKABLE void clearDevice();
    Q_INVOKABLE void setRotationMode(const QString &mode);
    Q_INVOKABLE void setFullHd(bool on);
    Q_INVOKABLE void setAudioOffset(int ms);

    QString rotationMode() const { return m_rotationMode; }
    bool fullHd() const { return m_fullHd; }
    int audioOffsetMs() const { return m_audioOffsetMs; }

signals:
    void statusChanged();
    void devicesChanged();

private slots:
    void poll();

private:
    QTimer  m_timer;
    QString m_state = QStringLiteral("idle");
    QString m_iface;
    int     m_frames = 0;
    int     m_attempts = 0;
    QVariantList m_devices;
    QString m_selectedMac;
    bool    m_scanned = false;
    QString m_targetName;
    QString m_rotationMode = QStringLiteral("auto");
    bool    m_fullHd = true;
    int     m_audioOffsetMs = 0;
};

#endif // CASTCONTROLLER_H
