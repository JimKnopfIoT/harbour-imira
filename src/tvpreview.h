/*
  harbour-imira — TvPreview
  Copyright (C) 2026  harbour-imira contributors — GPLv3 or later.

  Image provider for the phone-side live view of the TV desktop: reads the
  frame imira-comp publishes in /dev/shm/imira-comp-fb (see the compositor
  for the header layout) and hands it to QML as a QImage. The QML page polls
  a few times per second — a preview, not a second video pipeline.
*/
#ifndef TVPREVIEW_H
#define TVPREVIEW_H

#include <QQuickImageProvider>

class TvPreview : public QQuickImageProvider
{
public:
    TvPreview() : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;
};

#endif // TVPREVIEW_H
