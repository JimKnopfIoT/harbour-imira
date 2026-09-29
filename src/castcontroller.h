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
  state is one of idle/starting/scanning/connecting/handshake/streaming/
  reporting/error/nowlan ("handshake": Wi-Fi Direct is up, but no frame has
  gone out yet; "streaming" only once frames flow). Missing
  file or a malformed line reads as idle — the service simply is not running
  yet.

  Diagnostics (DiagnosticsPage): /tmp/imira-debug (flag) makes the service
  run its wpa_supplicant with -d from the next start on; /tmp/imira-report-
  request (contents: options, e.g. "survey") asks for a report. The service
  shows state "reporting" while it scans and collects, then writes the path
  of the anonymized report in ~/Documents to /tmp/imira-report-done and
  removes the debug flag again.

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
#include <QHash>
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
    // Audio route (see daemon audiocapture.cpp): "auto", "all" or
    // "<output>.monitor". audioRoute = chosen, audioRouteActive = what the
    // daemon runs right now ("" = not casting, "failed" = no audio),
    // audioRoutes = what cycleAudioRoute() steps through (the daemon lists
    // the outputs it found; before the first cast only auto/all).
    Q_PROPERTY(QString audioRoute READ audioRoute NOTIFY statusChanged)
    Q_PROPERTY(QString audioRouteActive READ audioRouteActive NOTIFY statusChanged)
    Q_PROPERTY(QStringList audioRoutes READ audioRoutes NOTIFY statusChanged)
    // false = mirror the phone screen (default); true = convergence: the TV
    // becomes its own virtual screen (imira-comp). Applies to the NEXT cast.
    Q_PROPERTY(bool convergence READ convergence NOTIFY statusChanged)
    // Convergence monitor: window titles currently on the TV, and the CPU
    // load (percent, all cores) of the second instance — compositor,
    // encoder and the TV apps together.
    Q_PROPERTY(QStringList tvWindows READ tvWindows NOTIFY statusChanged)
    Q_PROPERTY(int tvLoad READ tvLoad NOTIFY statusChanged)
    // htop, scoped to convergence: per-process {name, cpu} of compositor,
    // encoder and every TV app, sorted by load.
    Q_PROPERTY(QVariantList tvProcs READ tvProcs NOTIFY statusChanged)
    // Diagnostics: detailed supplicant log for the next cast, and the path
    // of the last report ("" = none yet).
    Q_PROPERTY(bool debugLog READ debugLog NOTIFY diagnosticsChanged)
    Q_PROPERTY(QString reportPath READ reportPath NOTIFY diagnosticsChanged)
    // Radio situation of the running cast (/tmp/imira-radio, written by the
    // service): "" (unknown/none), "alone", "shared", "hop" (the receiver
    // took another channel than the phone's Wi-Fi) or "dfs" (the phone's
    // Wi-Fi is on a radar channel the cast cannot share). With hop/dfs the
    // one radio has to switch between two channels, which costs packets.
    Q_PROPERTY(QString radioMode READ radioMode NOTIFY statusChanged)
    Q_PROPERTY(int wifiMhz READ wifiMhz NOTIFY statusChanged)
    Q_PROPERTY(int castMhz READ castMhz NOTIFY statusChanged)

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
    // Next audio route; applies live during a cast.
    Q_INVOKABLE void cycleAudioRoute();
    Q_INVOKABLE void setConvergence(bool on);
    Q_INVOKABLE void setDebugLog(bool on);
    // survey = also describe the radio environment (channels in use).
    Q_INVOKABLE void createReport(bool survey);
    // Whole report as text, for the clipboard.
    Q_INVOKABLE QString reportText() const;

    // The TV dock's app selection. installedApps lists every launcher app
    // as {id, name, icon}; tvApps/setTvApps read and write the selection
    // (~/.config/imira/tv-apps) that the compositor reloads live.
    // Saves the current TV frame as a PNG in Pictures/Screenshots and
    // returns the file name ("" if there is no frame).
    Q_INVOKABLE QString saveTvScreenshot() const;

    Q_INVOKABLE QVariantList installedApps() const;
    Q_INVOKABLE QStringList tvApps() const;
    Q_INVOKABLE void setTvApps(const QStringList &ids);

    QString rotationMode() const { return m_rotationMode; }
    bool fullHd() const { return m_fullHd; }
    int audioOffsetMs() const { return m_audioOffsetMs; }
    QString audioRoute() const { return m_audioRoute; }
    QString audioRouteActive() const { return m_audioRouteActive; }
    QStringList audioRoutes() const { return m_audioRoutes; }
    bool convergence() const { return m_convergence; }
    QStringList tvWindows() const { return m_tvWindows; }
    int tvLoad() const { return m_tvLoad; }
    QVariantList tvProcs() const { return m_tvProcs; }
    bool debugLog() const { return m_debugLog; }
    QString reportPath() const { return m_reportPath; }
    QString radioMode() const { return m_radioMode; }
    int wifiMhz() const { return m_wifiMhz; }
    int castMhz() const { return m_castMhz; }

signals:
    void statusChanged();
    void devicesChanged();
    void diagnosticsChanged();

private slots:
    void poll();

private:
    void updateTvMonitor();

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
    QString m_audioRoute = QStringLiteral("auto");
    QString m_audioRouteActive;
    QStringList m_audioRoutes;
    bool    m_convergence = false;
    QStringList m_tvWindows;
    int     m_tvLoad = 0;
    QVariantList m_tvProcs;
    bool    m_debugLog = false;
    QString m_reportPath;
    QString m_radioMode;
    int     m_wifiMhz = 0;
    int     m_castMhz = 0;
    QHash<qint64, qulonglong> m_lastJiffies;
    qint64  m_lastJiffiesMs = 0;
};

#endif // CASTCONTROLLER_H
