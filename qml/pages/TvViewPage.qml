/*
  harbour-imira — TvViewPage.qml
  Copyright (C) 2026  harbour-imira contributors — GPLv3 or later.

  Swipe left from the main page: a live view of the TV desktop plus an
  htop-style load list, scoped to the convergence instance (compositor,
  encoder and the TV apps). The preview polls the compositor's shared frame
  buffer a few times per second — a peek, not a second video pipeline.
*/
import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    property int frameSeq: 0
    // The live preview costs a frame copy twice a second — opt-in.
    property bool previewOn: false

    Timer {
        interval: 500
        repeat: true
        running: page.status === PageStatus.Active && cast.convergence
                 && page.previewOn
        onTriggered: page.frameSeq++
    }

    property string shotResult

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        PullDownMenu {
            MenuItem {
                text: qsTr("Save TV screenshot")
                enabled: cast.convergence
                onClicked: {
                    var name = cast.saveTvScreenshot()
                    page.shotResult = name !== ""
                        //: %1 is a file name
                        ? qsTr("Saved: %1").arg(name)
                        : qsTr("No TV picture to save")
                    resultTimer.restart()
                }
            }
        }

        Timer {
            id: resultTimer
            interval: 4000
            onTriggered: page.shotResult = ""
        }

        Column {
            id: column
            width: page.width
            spacing: Theme.paddingLarge

            PageHeader { title: qsTr("TV view") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: page.shotResult !== ""
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.highlightColor
                text: page.shotResult
            }

            TextSwitch {
                text: qsTr("Live preview")
                description: qsTr("Shows the TV picture here, updated twice "
                    + "per second.")
                automaticCheck: false
                checked: page.previewOn
                onClicked: page.previewOn = !page.previewOn
            }

            Image {
                id: preview
                visible: page.previewOn
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                height: page.previewOn ? width * 9 / 16 : 0
                fillMode: Image.PreserveAspectFit
                cache: false
                asynchronous: true
                source: cast.convergence && page.previewOn
                        ? "image://tvview/frame" + page.frameSeq : ""
                Rectangle {
                    anchors.fill: parent
                    color: "black"
                    visible: preview.status !== Image.Ready
                    Label {
                        anchors.centerIn: parent
                        width: parent.width - 2 * Theme.paddingLarge
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.secondaryColor
                        text: cast.convergence
                              ? qsTr("No picture — is a convergence cast "
                                     + "running?")
                              : qsTr("Convergence is off")
                    }
                }
            }

            SectionHeader { text: qsTr("Convergence load") }

            Repeater {
                model: cast.tvProcs

                delegate: Item {
                    width: page.width
                    height: Theme.itemSizeExtraSmall

                    Rectangle {
                        // htop-style bar behind the row, capped at 100 %.
                        x: Theme.horizontalPageMargin
                        anchors.verticalCenter: parent.verticalCenter
                        height: parent.height - Theme.paddingMedium
                        width: (page.width - 2 * Theme.horizontalPageMargin)
                               * Math.min(modelData.cpu, 100) / 100
                        radius: Theme.paddingSmall / 2
                        color: Theme.rgba(Theme.highlightColor,
                                          Theme.opacityFaint)
                    }
                    Label {
                        anchors {
                            left: parent.left
                            leftMargin: Theme.horizontalPageMargin
                                + Theme.paddingMedium
                            verticalCenter: parent.verticalCenter
                        }
                        text: modelData.name
                        truncationMode: TruncationMode.Fade
                        width: parent.width * 0.6
                    }
                    Label {
                        anchors {
                            right: parent.right
                            rightMargin: Theme.horizontalPageMargin
                                + Theme.paddingMedium
                            verticalCenter: parent.verticalCenter
                        }
                        //: %1 is a CPU percentage
                        text: qsTr("%1 %").arg(modelData.cpu)
                        color: Theme.highlightColor
                    }
                }
            }

            DetailItem {
                label: qsTr("Total")
                //: %1 is a CPU percentage
                value: qsTr("%1 %").arg(cast.tvLoad)
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                text: qsTr("100 % equals one fully used CPU core; the phone "
                    + "has eight.")
            }
        }

        VerticalScrollDecorator { }
    }
}
