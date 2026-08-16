/*
 * harbour-imira — frame conversion: RGBA screen frames to I420/NV12,
 * scaled and letterboxed into the encoder's target size.
 */
#ifndef IMIRA_CONVERT_H
#define IMIRA_CONVERT_H

#include <cstdint>
#include <cstddef>
#include <vector>

namespace imira {

class FrameConverter {
public:
    // dstFormat: true = NV12 (interleaved chroma), false = I420 (planar).
    void configure(int dstWidth, int dstHeight, bool nv12);

    // src is tightly packed R,G,B,A bytes (stride in bytes), optionally
    // vertically flipped (yInverted). rotation: 0, 90 or 270 (content
    // orientation inside the portrait buffer). Returns a malloc'd buffer of
    // dstWidth*dstHeight*3/2 bytes, ownership passes to the caller.
    // Source is scaled to fit, centred, black bars around it.
    uint8_t *convert(const uint8_t *src, int srcWidth, int srcHeight,
                     int srcStride, bool yInverted, int rotation,
                     size_t *outSize);

    int dstWidth() const { return m_dw; }
    int dstHeight() const { return m_dh; }

private:
    int m_dw = 0, m_dh = 0;
    bool m_nv12 = true;
    int m_boxX = 0, m_boxY = 0, m_boxW = 0, m_boxH = 0;
    int m_lastSrcW = 0, m_lastSrcH = 0, m_lastRot = -1;
    void updateMaps(int srcW, int srcH, int rot);
};

} // namespace imira

#endif
