/*
  harbour-imira — main.cpp
  Copyright (C) 2026  harbour-imira contributors — GPLv3 or later.

  Miracast screen mirroring for Sailfish OS. The UI is deliberately thin: all
  Wi-Fi Direct / RTSP / encoding work happens in the imira system service;
  this process only shows its status and writes two flag files (see
  castcontroller.h).
*/
#include <QGuiApplication>
#include <QQmlContext>
#include <QQuickView>
#include <QScopedPointer>

#include <sailfishapp.h>

#include "castcontroller.h"

int main(int argc, char *argv[])
{
    // SailfishApp::application() sets organisation/app name and the QML
    // import paths Silica needs; a bare QGuiApplication would leave Silica
    // unable to find its own theme.
    QScopedPointer<QGuiApplication> app(SailfishApp::application(argc, argv));
    app->setApplicationVersion(QStringLiteral(APP_VERSION));

    QScopedPointer<QQuickView> view(SailfishApp::createView());

    // Context property rather than a registered type: there is exactly one
    // controller and every page (cover included) shares its polled state.
    CastController controller;
    view->rootContext()->setContextProperty(QStringLiteral("cast"), &controller);

    view->setSource(SailfishApp::pathToMainQml());
    view->show();
    return app->exec();
}
