/*
  harbour-imira — MainPage.qml
  Copyright (C) 2026  harbour-imira contributors — GPLv3 or later.

  One screen: what the cast is doing, one button to flip it. `cast` is the
  CastController context property; its state is the service's word, not ours —
  after Start the page keeps saying Idle until the service actually picks the
  flag file up (at most one poll interval).
*/
import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    // Swipe left: the TV view with the convergence load monitor.
    onStatusChanged: {
        if (status === PageStatus.Active && !forwardNavigation)
            pageStack.pushAttached(Qt.resolvedUrl("TvViewPage.qml"))
    }

    // "running" = a cast is underway (scanning is not casting).
    readonly property bool running: cast.state === "starting"
                                 || cast.state === "connecting"
                                 || cast.state === "handshake"
                                 || cast.state === "streaming"
    readonly property bool busy: cast.state === "starting"
                              || cast.state === "connecting"
                              || cast.state === "handshake"
                              || cast.state === "scanning"
                              || cast.state === "reporting"

    function stateText() {
        switch (cast.state) {
        case "starting":   return qsTr("Starting")
        case "scanning":   return qsTr("Searching for devices…")
        case "connecting": return qsTr("Connecting")
        case "handshake":  return qsTr("Waiting for receiver")
        case "streaming":  return qsTr("Streaming")
        case "reporting":  return qsTr("Creating diagnostic report…")
        case "error":      return qsTr("Error")
        case "nowlan":     return qsTr("WLAN is off")
        default:
            if (cast.scanned && cast.devices.length === 0)
                return qsTr("No receiver found")
            return qsTr("Idle")
        }
    }

    function ifaceText() {
        if (cast.iface === "wlan1") return qsTr("Alfa (wlan1)")
        if (cast.iface === "p2p0")  return qsTr("internal (p2p0)")
        return cast.iface !== "" ? cast.iface : "–"
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        PullDownMenu {
            MenuItem {
                text: qsTr("About")
                onClicked: pageStack.push(Qt.resolvedUrl("AboutPage.qml"))
            }
            MenuItem {
                text: qsTr("TV apps")
                onClicked: pageStack.push(Qt.resolvedUrl("TvAppsPage.qml"))
            }
            MenuItem {
                text: qsTr("Scan for devices")
                // Scanning tears the P2P interface away from an active cast,
                // and a second scan on top of a running one only restarts it.
                enabled: !page.running && cast.state !== "scanning"
                         && cast.state !== "reporting"
                onClicked: cast.scan()
            }
        }

        Column {
            id: column
            width: page.width
            spacing: Theme.paddingLarge

            PageHeader { title: qsTr("Screen mirroring") }

            // --- big status ------------------------------------------------
            Item {
                width: parent.width
                height: Theme.itemSizeHuge

                BusyIndicator {
                    anchors.centerIn: parent
                    size: BusyIndicatorSize.Large
                    running: page.busy
                }
                Icon {
                    anchors.centerIn: parent
                    visible: !page.busy
                    source: cast.state === "streaming" ? "image://theme/icon-l-share"
                          : cast.state === "error"     ? "image://theme/icon-l-attention"
                                                       : "image://theme/icon-l-play"
                    highlighted: cast.state === "streaming"
                }
            }

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: stateText()
                font.pixelSize: Theme.fontSizeExtraLarge
                color: cast.state === "error" || cast.state === "nowlan"
                         ? Theme.errorColor
                     : cast.state === "streaming" ? Theme.highlightColor
                                                  : Theme.primaryColor
            }

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: cast.state === "nowlan"
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryColor
                text: qsTr("Casting needs the WLAN radio. Enable WLAN in the settings — a network connection is not required.")
            }

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: page.running && cast.targetName !== ""
                font.pixelSize: Theme.fontSizeMedium
                color: Theme.secondaryHighlightColor
                //: %1 is the name of the receiver being cast to
                text: qsTr("Target: %1").arg(cast.targetName)
            }

            // The phone has one radio. If its Wi-Fi and the cast sit on two
            // channels it has to switch back and forth, and packets get
            // lost — measured as the main cause of dropouts. Only while a
            // cast is running and only when it applies.
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: page.running
                         && (cast.radioMode === "dfs" || cast.radioMode === "hop")
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryHighlightColor
                text: cast.radioMode === "dfs"
                      //: %1 = frequency of the phone's Wi-Fi in MHz
                      ? qsTr("Your Wi-Fi uses a radar channel (%1 MHz) that the cast cannot share. "
                             + "This can cause dropouts: use a 2.4 GHz network or disconnect "
                             + "from Wi-Fi while casting.").arg(cast.wifiMhz)
                      //: %1 = receiver's channel, %2 = phone's Wi-Fi, both in MHz
                      : qsTr("The receiver chose another channel (%1 MHz) than your Wi-Fi "
                             + "(%2 MHz). This can cause dropouts: disconnect from Wi-Fi "
                             + "while casting if picture or sound break up.")
                            .arg(cast.castMhz).arg(cast.wifiMhz)
            }

            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                text: page.running ? qsTr("Stop casting") : qsTr("Start casting")
                // Stopping is always allowed; starting only when the radio is
                // usable and at least one receiver is actually known.
                enabled: page.running
                       || (cast.state !== "scanning" && cast.state !== "nowlan"
                           && cast.state !== "reporting"
                           && cast.devices.length > 0)
                onClicked: page.running ? cast.stop() : cast.start()
            }

            ComboBox {
                label: qsTr("Orientation")
                // Maps to the daemon's rotation override: auto = sensor,
                // 0 = portrait pinned, 90/270 = the two landscape
                // directions pinned (gyro games!) — which one is right
                // depends on which way the app lays itself down.
                currentIndex: cast.rotationMode === "0" ? 1
                            : cast.rotationMode === "90" ? 2
                            : cast.rotationMode === "270" ? 3 : 0
                menu: ContextMenu {
                    MenuItem { text: qsTr("Automatic") }
                    MenuItem { text: qsTr("Portrait") }
                    MenuItem { text: qsTr("Landscape") }
                    MenuItem { text: qsTr("Landscape (inverted)") }
                }
                onCurrentIndexChanged: {
                    var mode = currentIndex === 1 ? "0"
                             : currentIndex === 2 ? "90"
                             : currentIndex === 3 ? "270" : "auto"
                    if (mode !== cast.rotationMode)
                        cast.setRotationMode(mode)
                }
            }

            TextSwitch {
                text: qsTr("Full HD (1080p)")
                description: qsTr("Off: 720p — smoother on weak links. Switches live during casting.")
                automaticCheck: false
                checked: cast.fullHd
                onClicked: cast.setFullHd(!checked)
            }

            // --- convergence monitor --------------------------------------
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: cast.convergence && page.running
                         && cast.tvWindows.length > 0
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryHighlightColor
                //: %1 is a comma-separated list of app window titles
                text: qsTr("On the TV: %1").arg(cast.tvWindows.join(", "))
            }
            DetailItem {
                visible: cast.convergence && page.running
                label: qsTr("TV load (CPU)")
                //: %1 is a CPU percentage
                value: qsTr("%1 %").arg(cast.tvLoad)
            }

            TextSwitch {
                text: qsTr("Convergence (experimental)")
                description: qsTr("The TV becomes its own screen with app "
                    + "windows instead of mirroring the phone. Takes effect "
                    + "on the next cast start.")
                automaticCheck: false
                checked: cast.convergence
                onClicked: cast.setConvergence(!checked)
            }

            Slider {
                width: parent.width
                label: qsTr("Audio delay")
                //: Slider value, %1 is a number of milliseconds
                valueText: qsTr("%1 ms").arg(Math.round(sliderValue))
                minimumValue: -2000
                maximumValue: 2000
                stepSize: 25
                // Deliberately not bound to cast.audioOffsetMs — the poll
                // echoing the value back would fight the drag. Seed once,
                // then the slider is the source of truth.
                Component.onCompleted: value = cast.audioOffsetMs
                // Applies live during casting (the daemon polls the value);
                // positive = audio later, negative = audio earlier.
                onSliderValueChanged: cast.setAudioOffset(Math.round(sliderValue))
            }

            // Audio route: tap steps through auto, all and a direct copy of
            // every output the daemon found. Applies live; every switch and
            // what each stream did lands in the audio log of the report.
            ValueButton {
                function outputName(route) {
                    var s = route.replace(/\.monitor$/, "")
                    if (s === "sink.deep_buffer") return qsTr("phone media output")
                    if (s === "sink.primary_output") return qsTr("phone system output")
                    if (s === "sink.fast") return qsTr("phone low-latency output")
                    if (s.indexOf("bluez_sink.") === 0) return qsTr("Bluetooth")
                    return s
                }
                readonly property string route: cast.audioRoute
                readonly property bool failed: page.running
                                               && cast.audioRouteActive === "failed"
                label: qsTr("Audio route")
                value: route === "auto" ? qsTr("Automatic")
                     : route === "all" ? qsTr("Everything to the TV")
                     : qsTr("Copy of %1").arg(outputName(route))
                description: (route === "auto"
                        ? qsTr("Media from the phone plays on the TV, the phone "
                               + "stays silent. If the TV stays silent, tap to "
                               + "try the next route.")
                        : route === "all"
                        ? qsTr("Every sound goes to the TV, including Bluetooth, "
                               + "ringtones and alarms.")
                        : qsTr("The TV gets a copy of %1 (%2), which keeps "
                               + "playing there too.")
                              .arg(outputName(route))
                              .arg(route.replace(/\.monitor$/, "")))
                    + (failed ? " " + qsTr("This route does not work, the TV "
                                           + "gets no audio.") : "")
                onClicked: cast.cycleAudioRoute()
            }

            // --- receivers -------------------------------------------------
            SectionHeader { text: qsTr("Receivers") }

            // "Automatic" is always first: no /tmp/imira-peer file means the
            // service connects to the first Miracast sink it finds.
            BackgroundItem {
                width: page.width
                height: Theme.itemSizeMedium
                onClicked: cast.clearDevice()

                Icon {
                    id: autoCheck
                    anchors {
                        left: parent.left
                        leftMargin: Theme.horizontalPageMargin
                        verticalCenter: parent.verticalCenter
                    }
                    source: "image://theme/icon-s-accept"
                    visible: cast.selectedMac === ""
                }
                Column {
                    anchors {
                        left: parent.left
                        leftMargin: Theme.horizontalPageMargin
                            + Theme.iconSizeSmall + Theme.paddingMedium
                        right: parent.right
                        rightMargin: Theme.horizontalPageMargin
                        verticalCenter: parent.verticalCenter
                    }
                    Label {
                        width: parent.width
                        truncationMode: TruncationMode.Fade
                        text: qsTr("Automatic")
                        color: cast.selectedMac === "" ? Theme.highlightColor
                                                       : Theme.primaryColor
                    }
                    Label {
                        width: parent.width
                        truncationMode: TruncationMode.Fade
                        text: qsTr("first sink found")
                        font.pixelSize: Theme.fontSizeExtraSmall
                        color: Theme.secondaryColor
                    }
                }
            }

            Repeater {
                model: cast.devices

                delegate: BackgroundItem {
                    width: page.width
                    // A P2P peer without Wi-Fi Display (a printer, a laptop)
                    // answers the scan too, but is no receiver — hide it.
                    visible: modelData.wfd
                    height: visible ? Theme.itemSizeMedium : 0
                    enabled: modelData.wfd
                    onClicked: cast.selectDevice(modelData.mac)

                    Icon {
                        anchors {
                            left: parent.left
                            leftMargin: Theme.horizontalPageMargin
                            verticalCenter: parent.verticalCenter
                        }
                        source: "image://theme/icon-s-accept"
                        visible: cast.selectedMac === modelData.mac
                    }
                    Column {
                        anchors {
                            left: parent.left
                            leftMargin: Theme.horizontalPageMargin
                                + Theme.iconSizeSmall + Theme.paddingMedium
                            right: parent.right
                            rightMargin: Theme.horizontalPageMargin
                            verticalCenter: parent.verticalCenter
                        }
                        Label {
                            width: parent.width
                            truncationMode: TruncationMode.Fade
                            text: modelData.name !== "" ? modelData.name
                                                        : modelData.mac
                            color: !modelData.wfd ? Theme.secondaryColor
                                 : cast.selectedMac === modelData.mac
                                   ? Theme.highlightColor : Theme.primaryColor
                        }
                        Label {
                            width: parent.width
                            truncationMode: TruncationMode.Fade
                            text: modelData.wfd ? qsTr("Miracast")
                                                : qsTr("no Miracast receiver")
                            font.pixelSize: Theme.fontSizeExtraSmall
                            color: Theme.secondaryColor
                        }
                    }
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: page.width - 2 * Theme.horizontalPageMargin
                visible: cast.devices.length === 0 && cast.state !== "scanning"
                wrapMode: Text.WordWrap
                text: qsTr("Pull down to scan for devices")
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryColor
            }

            Item { width: 1; height: Theme.paddingLarge }

            // --- details ---------------------------------------------------
            DetailItem {
                label: qsTr("Interface")
                value: ifaceText()
            }
            DetailItem {
                label: qsTr("Frames")
                value: cast.frames
            }
            DetailItem {
                label: qsTr("Connection attempts")
                value: cast.attempts
            }
        }

        VerticalScrollDecorator { }
    }
}
