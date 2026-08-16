/*
  harbour-imira — TvAppsPage.qml
  Copyright (C) 2026  harbour-imira contributors — GPLv3 or later.

  Which apps appear in the TV dock (convergence mode). Every toggle writes
  the selection immediately; the compositor reloads it live, so the dock on
  the TV follows within seconds — no cast restart.
*/
import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    property var selection: cast.tvApps()

    function toggle(appId) {
        var s = selection.slice()
        var i = s.indexOf(appId)
        if (i >= 0)
            s.splice(i, 1)
        else
            s.push(appId)
        selection = s
        cast.setTvApps(s)
    }

    SilicaListView {
        anchors.fill: parent
        model: cast.installedApps()

        header: Column {
            width: page.width
            PageHeader { title: qsTr("TV apps") }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryColor
                text: qsTr("These apps appear in the dock on the TV. Changes "
                    + "apply to a running cast within a few seconds.")
            }
            Item { width: 1; height: Theme.paddingMedium }
        }

        delegate: BackgroundItem {
            width: page.width
            height: Theme.itemSizeSmall
            onClicked: page.toggle(modelData.id)

            Image {
                id: icon
                anchors {
                    left: parent.left
                    leftMargin: Theme.horizontalPageMargin
                    verticalCenter: parent.verticalCenter
                }
                width: Theme.iconSizeMedium
                height: Theme.iconSizeMedium
                sourceSize.width: width
                sourceSize.height: height
                fillMode: Image.PreserveAspectFit
                source: modelData.icon.charAt(0) === "/"
                        ? "file://" + modelData.icon
                        : "image://theme/" + modelData.icon
            }
            Label {
                anchors {
                    left: icon.right
                    leftMargin: Theme.paddingLarge
                    right: check.left
                    rightMargin: Theme.paddingMedium
                    verticalCenter: parent.verticalCenter
                }
                truncationMode: TruncationMode.Fade
                text: modelData.name
                color: page.selection.indexOf(modelData.id) >= 0
                       ? Theme.highlightColor : Theme.primaryColor
            }
            Switch {
                id: check
                anchors {
                    right: parent.right
                    rightMargin: Theme.horizontalPageMargin
                    verticalCenter: parent.verticalCenter
                }
                automaticCheck: false
                checked: page.selection.indexOf(modelData.id) >= 0
                onClicked: page.toggle(modelData.id)
            }
        }

        VerticalScrollDecorator { }
    }
}
