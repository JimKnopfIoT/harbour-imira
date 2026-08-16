/*
  harbour-imira — CoverPage.qml
  Copyright (C) 2026  harbour-imira contributors — GPLv3 or later.

  State and frame counter, plus one action: start when resting, stop when
  anything is underway. The cover is where a running cast will mostly be
  watched from, so the frame counter doubles as a liveness indicator.
*/
import QtQuick 2.0
import Sailfish.Silica 1.0

CoverBackground {
    id: cover

    readonly property bool running: cast.state === "starting"
                                 || cast.state === "connecting"
                                 || cast.state === "streaming"

    function stateText() {
        switch (cast.state) {
        case "starting":   return qsTr("Starting")
        case "scanning":   return qsTr("Scanning")
        case "connecting": return qsTr("Connecting")
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
    }

    CoverActionList {
        CoverAction {
            iconSource: cover.running ? "image://theme/icon-cover-cancel"
                                      : "image://theme/icon-cover-play"
            onTriggered: cover.running ? cast.stop() : cast.start()
        }
    }
}
