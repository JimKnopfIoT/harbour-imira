/*
  harbour-imira — TvPreview
  Copyright (C) 2026  harbour-imira contributors — GPLv3 or later.
*/
#include "tvpreview.h"

#include <QFile>

namespace {
struct ShmHeader {
    quint32 magic;
    quint32 seq;
    quint32 width;
    quint32 height;
};
constexpr quint32 kMagic = 0x31464349; // "ICF1"
} // namespace

QImage TvPreview::requestImage(const QString &, QSize *size,
                               const QSize &requestedSize)
{
    QFile f(QStringLiteral("/dev/shm/imira-comp-fb"));
    if (!f.open(QIODevice::ReadOnly))
        return QImage();
    const QByteArray data = f.readAll();
    if ((size_t)data.size() < sizeof(ShmHeader))
        return QImage();
    const auto *hdr = reinterpret_cast<const ShmHeader *>(data.constData());
    if (hdr->magic != kMagic || hdr->width == 0 || hdr->height == 0)
        return QImage();
    if ((quint32)data.size() < sizeof(ShmHeader) + hdr->width * hdr->height * 4)
        return QImage();

    QImage img(reinterpret_cast<const uchar *>(data.constData())
                   + sizeof(ShmHeader),
               hdr->width, hdr->height, hdr->width * 4,
               QImage::Format_RGBA8888);
    // GL readback is bottom-up; detach from the temporary buffer via the
    // mirror copy in one go.
    QImage result = img.mirrored(false, true);
    if (requestedSize.isValid())
        result = result.scaled(requestedSize, Qt::KeepAspectRatio,
                               Qt::SmoothTransformation);
    if (size)
        *size = result.size();
    return result;
}
