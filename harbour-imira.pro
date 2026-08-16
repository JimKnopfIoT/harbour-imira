# harbour-imira — Miracast screen mirroring for Sailfish OS (UI app).
#
# This .pro builds ONLY the Silica UI. The streaming daemon in daemon/ is
# built separately (daemon/build-castd.sh, direct sb2 compile) and is merely
# packaged by rpm/harbour-imira.spec — do not add daemon/ sources here.
#
# The UI talks to the system service through files, not D-Bus:
#   /tmp/imira-start, /tmp/imira-stop   flag files written by the app
#   /tmp/imira-status                    one line "state frames attempts iface",
#                                        written by the service, polled at 1 Hz
# so the app itself needs no privileges at all.

TARGET = harbour-imira

CONFIG += sailfishapp
QT += core qml quick

# The rpm build passes these (%qmake5 VERSION=%{version}); fallbacks keep a
# standalone/IDE build from showing an empty version on the About page.
isEmpty(VERSION): VERSION = 0.1.0
isEmpty(RELEASE): RELEASE = 1
DEFINES += APP_VERSION=\\\"$$VERSION\\\"
DEFINES += APP_RELEASE=\\\"$$RELEASE\\\"

HEADERS += \
    src/castcontroller.h \
    src/tvpreview.h

SOURCES += \
    src/harbour-imira.cpp \
    src/castcontroller.cpp \
    src/tvpreview.cpp

# Icons: four sizes under icons/<size>/harbour-imira.png; sailfishapp.prf
# installs them to /usr/share/icons/hicolor/<size>/apps/.
SAILFISHAPP_ICONS = 86x86 108x108 128x128 172x172

# --- Translations -----------------------------------------------------------
# English source strings + German (du-Form). libsailfishapp loads the .qm
# matching the device locale; anything else falls back to the English text.
CONFIG += sailfishapp_i18n
TRANSLATIONS += translations/harbour-imira-de.ts
lupdate_only {
    SOURCES += qml/*.qml qml/pages/*.qml qml/cover/*.qml
}

DISTFILES += \
    qml/harbour-imira.qml \
    qml/cover/CoverPage.qml \
    qml/pages/MainPage.qml \
    qml/pages/AboutPage.qml \
    qml/pages/TvAppsPage.qml \
    qml/pages/TvViewPage.qml \
    rpm/harbour-imira.spec \
    harbour-imira.desktop \
    translations/*.ts
