/*
  harbour-imira — AboutPage.qml
  Copyright (C) 2026  harbour-imira contributors — GPLv3 or later.
*/
import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column
            width: page.width
            spacing: Theme.paddingLarge

            PageHeader { title: qsTr("About") }

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Imira"
                font.pixelSize: Theme.fontSizeExtraLarge
                color: Theme.highlightColor
            }

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                //: %1 is the version number
                text: qsTr("Version %1").arg(Qt.application.version)
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryColor
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                text: qsTr("Miracast screen mirroring for Sailfish OS")
                color: Theme.primaryColor
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryColor
                text: qsTr("License: GNU GPLv3. Parts of the Wi-Fi Direct and "
                    + "screen recording code are derived from the aethercast "
                    + "and screencast projects.")
            }
        }

        VerticalScrollDecorator { }
    }
}
