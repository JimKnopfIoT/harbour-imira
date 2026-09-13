/*
 * harbour-imira — RGBA -> YUV420 conversion (BT.601 limited range),
 * nearest-neighbour scaling with letterbox and optional 90/270 degree
 * rotation (lipstick always delivers the native portrait framebuffer,
 * landscape content arrives rotated inside it). Plain C++; fast enough
 * for 720p30 on the Xperia 10 III.
 */
#include "convert.h"

#include <cstdlib>
#include <cstring>
#include <sys/resource.h>
#include <thread>
#include <vector>

namespace imira {

void FrameConverter::configure(int dstWidth, int dstHeight, bool nv12)
{
    // Leave headroom: the encoder, the capture and PulseAudio all need a
    // core too, and starving the audio thread is what people actually hear.
    unsigned hw = std::thread::hardware_concurrency();
    m_threads = hw >= 8 ? 4 : (hw >= 4 ? 2 : 1);

    m_dw = dstWidth & ~1;
    m_dh = dstHeight & ~1;
    m_nv12 = nv12;
    m_lastSrcW = m_lastSrcH = 0;
    m_lastRot = -1;
}

void FrameConverter::updateMaps(int srcW, int srcH, int rot)
{
    // Effective content size after rotation.
    int cw = (rot == 90 || rot == 270) ? srcH : srcW;
    int ch = (rot == 90 || rot == 270) ? srcW : srcH;
    int boxW = m_dw;
    int boxH = (int)((int64_t)m_dw * ch / cw);
    if (boxH > m_dh) {
        boxH = m_dh;
        boxW = (int)((int64_t)m_dh * cw / ch);
    }
    m_boxW = boxW & ~1;
    m_boxH = boxH & ~1;
    m_boxX = ((m_dw - m_boxW) / 2) & ~1;
    m_boxY = ((m_dh - m_boxH) / 2) & ~1;
    m_lastSrcW = srcW;
    m_lastSrcH = srcH;
    m_lastRot = rot;

    // Box filter sample counts (see convert()): round UP, cap at 4.
    m_sxN = (cw + m_boxW - 1) / (m_boxW > 0 ? m_boxW : 1);
    m_syN = (ch + m_boxH - 1) / (m_boxH > 0 ? m_boxH : 1);
    if (m_sxN < 1) m_sxN = 1;
    if (m_sxN > 4) m_sxN = 4;
    if (m_syN < 1) m_syN = 1;
    if (m_syN > 4) m_syN = 4;

    // Source coordinate per destination coordinate. Done once per geometry
    // instead of once per pixel — the division used to dominate the cost.
    m_mapX.resize(m_boxW);
    for (int x = 0; x < m_boxW; x++)
        m_mapX[x] = (int)((int64_t)x * cw / m_boxW);
    m_mapY.resize(m_boxH);
    for (int y = 0; y < m_boxH; y++)
        m_mapY[y] = (int)((int64_t)y * ch / m_boxH);
}

uint8_t *FrameConverter::convert(const uint8_t *src, int srcW, int srcH,
                                 int srcStride, bool yInverted, int rotation,
                                 size_t *outSize)
{
    if (srcW != m_lastSrcW || srcH != m_lastSrcH || rotation != m_lastRot)
        updateMaps(srcW, srcH, rotation);

    const size_t ySize = (size_t)m_dw * m_dh;
    const size_t size = ySize * 3 / 2;
    uint8_t *dst = static_cast<uint8_t *>(malloc(size));
    if (!dst)
        return nullptr;

    // Black in YUV limited range: Y=16, U=V=128.
    memset(dst, 16, ySize);
    memset(dst + ySize, 128, size - ySize);

    uint8_t *dstY = dst;
    uint8_t *dstC = dst + ySize;                 // NV12: interleaved UV
    uint8_t *dstU = dst + ySize;                 // I420: U then V
    uint8_t *dstV = dst + ySize + ySize / 4;

    const int cw = (rotation == 90 || rotation == 270) ? srcH : srcW;
    const int ch = (rotation == 90 || rotation == 270) ? srcW : srcH;

    // Box filter: averaging the source region per output pixel. A single
    // nearest-neighbour tap turns Silica's fine dither texture into coarse
    // visible moiré on the TV; averaging cancels it. Sample counts are
    // capped to keep the cost bounded at high downscale factors; they and
    // the coordinate tables come from updateMaps().
    const int sxN = m_sxN;
    const int syN = m_syN;
    const int nSamples = sxN * syN;
    const int *mapX = m_mapX.data();

    auto renderRows = [&](int yFrom, int yTo) {
    for (int y = yFrom; y < yTo; y++) {
        // Position inside the rotated content raster.
        const int ry = m_mapY[y];
        uint8_t *outY = dstY + (size_t)(m_boxY + y) * m_dw + m_boxX;
        const bool chromaRow = ((y & 1) == 0);

        // Unrotated content is read row by row, so each sample row's start
        // address is computed once instead of once per pixel.
        const uint8_t *rows[4] = { nullptr, nullptr, nullptr, nullptr };
        if (rotation == 0) {
            for (int j = 0; j < syN; j++) {
                int cry = ry + j;
                if (cry >= ch)
                    cry = ch - 1;
                const int sy = yInverted ? srcH - 1 - cry : cry;
                rows[j] = src + (size_t)sy * srcStride;
            }
        }

        for (int x = 0; x < m_boxW; x++) {
            const int rx = mapX[x];
            int r = 0, g = 0, b = 0;
            if (rotation == 0) {
                for (int j = 0; j < syN; j++) {
                    for (int i = 0; i < sxN; i++) {
                        int crx = rx + i;
                        if (crx >= cw)
                            crx = cw - 1;
                        const uint8_t *p = rows[j] + (size_t)crx * 4;
                        r += p[0];
                        g += p[1];
                        b += p[2];
                    }
                }
            } else {
                for (int j = 0; j < syN; j++) {
                    int cry = ry + j;
                    if (cry >= ch)
                        cry = ch - 1;
                    for (int i = 0; i < sxN; i++) {
                        int crx = rx + i;
                        if (crx >= cw)
                            crx = cw - 1;
                        int sx, sy;
                        if (rotation == 90) { // rotated CW in buffer -> CCW
                            sx = cry;
                            sy = srcH - 1 - crx;
                        } else {              // 270
                            sx = srcW - 1 - cry;
                            sy = crx;
                        }
                        if (yInverted)
                            sy = srcH - 1 - sy;
                        const uint8_t *p =
                            src + (size_t)sy * srcStride + (size_t)sx * 4;
                        r += p[0];
                        g += p[1];
                        b += p[2];
                    }
                }
            }
            if (nSamples > 1) {
                r /= nSamples;
                g /= nSamples;
                b /= nSamples;
            }
            outY[x] = (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            if (chromaRow && ((x & 1) == 0)) {
                int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                int cy2 = (m_boxY + y) / 2;
                int cx2 = (m_boxX + x) / 2;
                if (m_nv12) {
                    uint8_t *c = dstC + (size_t)cy2 * m_dw + cx2 * 2;
                    c[0] = (uint8_t)u;
                    c[1] = (uint8_t)v;
                } else {
                    dstU[(size_t)cy2 * (m_dw / 2) + cx2] = (uint8_t)u;
                    dstV[(size_t)cy2 * (m_dw / 2) + cx2] = (uint8_t)v;
                }
            }
        }
    }
    };

    // The rows are independent — each writes its own luma line, and chroma
    // only on even rows, again one line each. So the work splits across
    // cores without any locking; on an eight core phone this is what turns
    // a frame that barely fits into the budget into one that comfortably
    // does. Bands start on an even row so no chroma line is shared.
    int bands = m_threads;
    if (bands > 1 && m_boxH >= 4 * bands) {
        std::vector<std::thread> workers;
        workers.reserve(bands - 1);
        const int rows = ((m_boxH / bands) + 1) & ~1;
        for (int b = 1; b < bands; b++) {
            const int y0 = b * rows;
            if (y0 >= m_boxH)
                break;
            // The last band always runs to the end, so no row is lost when
            // the height does not divide evenly.
            const int y1 = (b == bands - 1 || (b + 1) * rows > m_boxH)
                               ? m_boxH
                               : (b + 1) * rows;
            workers.emplace_back([&renderRows, y0, y1]() {
                // Colour conversion is the heaviest thing this process
                // does, and it must never take a core away from audio —
                // a dropped frame goes unnoticed, a gap in the sound does
                // not. Nice values are per thread on Linux.
                setpriority(PRIO_PROCESS, 0, 5);
                renderRows(y0, y1);
            });
        }
        renderRows(0, rows < m_boxH ? rows : m_boxH);
        for (auto &w : workers)
            w.join();
    } else {
        renderRows(0, m_boxH);
    }

    if (outSize)
        *outSize = size;
    return dst;
}

} // namespace imira
