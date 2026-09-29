/*
  harbour-imira — CoverPage.qml
  Copyright (C) 2026  harbour-imira contributors — GPLv3 or later.

  State and frame counter, plus one action: start when resting, stop when
  anything is underway. The cover is where a running cast will mostly be
  watched from, so the frame counter doubles as a liveness indicator.
  While streaming, a second action steps through the audio routes — from
  the cover, the app playing stays in front (Android apps pause in the
  background).
*/
import QtQuick 2.0
import Sailfish.Silica 1.0

CoverBackground {
    id: cover

    readonly property bool running: cast.state === "starting"
                                 || cast.state === "connecting"
                                 || cast.state === "handshake"
                                 || cast.state === "streaming"

    function routeText() {
        var r = cast.audioRoute
        if (r === "auto") return qsTr("Automatic")
        if (r === "all") return qsTr("Everything to the TV")
        r = r.replace(/\.monitor$/, "")
        if (r === "sink.deep_buffer") return qsTr("phone media output")
        if (r === "sink.primary_output") return qsTr("phone system output")
        if (r === "sink.fast") return qsTr("phone low-latency output")
        if (r.indexOf("bluez_sink.") === 0) return qsTr("Bluetooth")
        return r
    }

    function stateText() {
        switch (cast.state) {
        case "starting":   return qsTr("Starting")
        case "scanning":   return qsTr("Scanning")
        case "connecting": return qsTr("Connecting")
        case "handshake":  return qsTr("Waiting for receiver")
        case "streaming":  return qsTr("Streaming")
        case "error":      return qsTr("Error")
        case "nowlan":     return qsTr("WLAN off")
        default:           return qsTr("Idle")
        }
    }

    Column {
        anchors.centerIn: parent
        width: parent.width - 2 * Theme.paddingLarge
        spacing: Theme.paddingMedium

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Imira"
            color: Theme.highlightColor
            font.pixelSize: Theme.fontSizeSmall
        }
        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: stateText()
            font.pixelSize: Theme.fontSizeLarge
        }
        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: cast.state === "streaming"
            //: %1 is the number of transmitted frames
            text: qsTr("%1 frames").arg(cast.frames)
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.secondaryColor
        }
        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            visible: cast.state === "streaming"
            wrapMode: Text.Wrap
            //: Cover, current audio route; %1 is its name
            text: qsTr("Audio: %1").arg(routeText())
                  + (cast.audioRouteActive === "failed" ? " ✗" : "")
            font.pixelSize: Theme.fontSizeExtraSmall
            color: Theme.secondaryColor
        }
    }

    // Two lists, one active at a time: resting or setting up = one centred
    // start/stop action; streaming = stop plus the audio route switch.
    CoverActionList {
        enabled: cast.state !== "streaming"
        CoverAction {
            iconSource: cover.running ? "image://theme/icon-cover-cancel"
                                      : "image://theme/icon-cover-play"
            onTriggered: cover.running ? cast.stop() : cast.start()
        }
    }
    CoverActionList {
        enabled: cast.state === "streaming"
        CoverAction {
            iconSource: "image://theme/icon-cover-cancel"
            onTriggered: cast.stop()
        }
        CoverAction {
            iconSource: "image://theme/icon-cover-next-song"
            onTriggered: cast.cycleAudioRoute()
        }
    }
}
