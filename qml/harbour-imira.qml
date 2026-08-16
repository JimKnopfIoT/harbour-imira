/*
  harbour-imira — application root.
  Copyright (C) 2026  harbour-imira contributors — GPLv3 or later.
*/
import QtQuick 2.0
import Sailfish.Silica 1.0
import "pages"

ApplicationWindow {
    id: app

    initialPage: Component { MainPage { } }
    cover: Qt.resolvedUrl("cover/CoverPage.qml")

    // Casting mirrors the screen as-is; portrait keeps the UI predictable
    // while the sink shows whatever the compositor renders.
    allowedOrientations: Orientation.Portrait
}
