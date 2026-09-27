/*
  harbour-imira — DiagnosticsPage.qml
  Copyright (C) 2026  harbour-imira contributors — GPLv3 or later.

  For receivers nobody has tested yet: switch on the detailed log, try a
  cast, then let the service collect an anonymized report (it also searches
  for receivers for ~15 s and records what each one says about itself). The
  report itself is English on purpose — it goes to people all over the world.
*/
import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    readonly property bool casting: cast.state === "starting"
                                 || cast.state === "connecting"
                                 || cast.state === "handshake"
                                 || cast.state === "streaming"
    readonly property bool collecting: cast.state === "reporting"
    property bool requested: false

    Connections {
        target: cast
        onDiagnosticsChanged: {
            if (cast.reportPath !== "")
                page.requested = false
            copyButton.text = qsTr("Copy to clipboard")
        }
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column
            width: page.width
            spacing: Theme.paddingMedium

            PageHeader { title: qsTr("Diagnostics") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.highlightColor
                text: qsTr("Casting does not work with your receiver? A report "
                    + "shows what happened. Post it in the forum or on "
                    + "GitHub — it is in English so that anyone can read it.")
            }

            SectionHeader { text: qsTr("1. Record") }

            TextSwitch {
                text: qsTr("Detailed log")
                description: qsTr("Records the Wi-Fi Direct negotiation in full "
                    + "detail. Takes effect from the next cast and switches "
                    + "itself off once a report has been created.")
                checked: cast.debugLog
                automaticCheck: false
                onClicked: cast.setDebugLog(!cast.debugLog)
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryHighlightColor
                text: qsTr("Then switch on the receiver, start a cast, wait "
                    + "about 30 seconds, stop it and come back here.")
            }

            SectionHeader { text: qsTr("2. Create report") }

            TextSwitch {
                id: surveySwitch
                text: qsTr("Include radio environment")
                description: qsTr("Adds the channel of your Wi-Fi connection "
                    + "and how busy each channel is (number of networks and "
                    + "signal strength, no network names).")
            }

            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Create report")
                enabled: !page.casting && !page.collecting && !page.requested
                onClicked: {
                    page.requested = true
                    cast.createReport(surveySwitch.checked)
                }
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Theme.paddingMedium
                visible: page.collecting || page.requested
                BusyIndicator {
                    size: BusyIndicatorSize.Small
                    running: parent.visible
                    anchors.verticalCenter: parent.verticalCenter
                }
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.highlightColor
                    text: qsTr("Searching for receivers and collecting…")
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryHighlightColor
                visible: page.casting
                text: qsTr("Stop casting first.")
            }

            Column {
                width: parent.width
                spacing: Theme.paddingMedium
                visible: cast.reportPath !== "" && !page.requested

                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    wrapMode: Text.WrapAnywhere
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.highlightColor
                    //: %1 is the file name of the report in the Documents folder
                    text: qsTr("Saved in Documents: %1")
                          .arg(cast.reportPath.replace(/^.*\//, ""))
                }

                Button {
                    id: copyButton
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Copy to clipboard")
                    onClicked: {
                        Clipboard.text = cast.reportText()
                        text = qsTr("Copied")
                    }
                }

                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: Theme.secondaryColor
                    text: qsTr("The report is anonymized: MAC addresses are cut "
                        + "to the manufacturer part, names of other devices and "
                        + "networks are removed. Please look it over before "
                        + "posting it.")
                }
            }
        }

        VerticalScrollDecorator { }
    }
}
