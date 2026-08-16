/*
 * harbour-imira — H.264 hardware encoder via droidmedia.
 */
#include "encoder.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>

#include <droidmedia/droidmedia.h>
#include <droidmedia/droidmediacodec.h>
#include <droidmedia/droidmediaconstants.h>

// OMX_VIDEO_AVCPROFILETYPE / OMX_VIDEO_AVCLEVELTYPE (OpenMAX IL)
#define OMX_VIDEO_AVC_PROFILE_BASELINE 0x01
#define OMX_VIDEO_AVC_LEVEL_31 0x200

namespace imira {

namespace {

struct FrameCtx {
    uint8_t *data;
};

void frameRef(void *) {}

void frameUnref(void *opaque) {
    FrameCtx *ctx = static_cast<FrameCtx *>(opaque);
    free(ctx->data);
    delete ctx;
}

void onDataAvailable(void *opaque, DroidMediaCodecData *encoded) {
    H264Encoder::OutputCallback *cb =
        static_cast<H264Encoder::OutputCallback *>(opaque);
    if (*cb) {
        (*cb)(static_cast<const uint8_t *>(encoded->data.data),
              encoded->data.size, encoded->ts, encoded->sync,
              encoded->codec_config);
    }
}

void onError(void *, int err) {
    fprintf(stderr, "imira-castd: encoder error %d\n", err);
}

void onEos(void *) {}

int onSizeChanged(void *, int32_t, int32_t) { return 0; }

} // namespace

bool H264Encoder::init(int width, int height, int fps, int bitrate,
                       const OutputCallback &cb)
{
    if (!droid_media_init()) {
        m_error = "droid_media_init failed";
        return false;
    }
    m_out = cb;
    m_width = width;
    m_height = height;
    m_fps = fps;
    m_bitrate = bitrate;

    DroidMediaCodecEncoderMetaData meta;
    memset(&meta, 0, sizeof(meta));
    meta.parent.type = "video/avc";
    meta.parent.width = width;
    meta.parent.height = height;
    meta.parent.fps = fps;
    meta.parent.flags = DROID_MEDIA_CODEC_HW_ONLY;

    // Pick an input colour format the encoder supports. We can emit I420 or
    // NV12 from the converter; prefer planar, fall back to semi-planar.
    DroidMediaColourFormatConstants c;
    droid_media_colour_format_constants_init(&c);
    uint32_t formats[32];
    unsigned int n = droid_media_codec_get_supported_color_formats(
        &meta.parent, 1, formats, 32);
    int chosen = -1;
    for (unsigned int i = 0; i < n && chosen < 0; i++) {
        if ((int)formats[i] == c.OMX_COLOR_FormatYUV420Planar) {
            chosen = c.OMX_COLOR_FormatYUV420Planar;
            m_inputFormat = I420;
        }
    }
    for (unsigned int i = 0; i < n && chosen < 0; i++) {
        if ((int)formats[i] == c.OMX_COLOR_FormatYUV420SemiPlanar) {
            chosen = c.OMX_COLOR_FormatYUV420SemiPlanar;
            m_inputFormat = NV12;
        }
    }
    if (chosen < 0) {
        // Nothing matched; try semi-planar anyway (most common on Qualcomm).
        chosen = c.OMX_COLOR_FormatYUV420SemiPlanar;
        m_inputFormat = NV12;
        fprintf(stderr, "imira-castd: no known colour format among %u "
                        "reported, trying NV12\n", n);
    }

    meta.color_format = chosen;
    meta.bitrate = bitrate;
    meta.meta_data = 0;          // we pass raw CPU buffers
    meta.stride = width;
    meta.slice_height = height;
    meta.max_input_size = width * height * 3 / 2;
    meta.bitrate_mode = DROID_MEDIA_CODEC_BITRATE_CONTROL_VBR;
    meta.codec_specific.h264.profile = OMX_VIDEO_AVC_PROFILE_BASELINE;
    meta.codec_specific.h264.level = OMX_VIDEO_AVC_LEVEL_31;
    meta.codec_specific.h264.prepend_header_to_sync_frames = 1;

    m_codec = droid_media_codec_create_encoder(&meta);
    if (!m_codec) {
        m_error = "droid_media_codec_create_encoder failed";
        return false;
    }

    static DroidMediaCodecCallbacks ccb;
    ccb.signal_eos = onEos;
    ccb.error = onError;
    ccb.size_changed = onSizeChanged;
    droid_media_codec_set_callbacks(m_codec, &ccb, nullptr);

    static DroidMediaCodecDataCallbacks dcb;
    dcb.data_available = onDataAvailable;
    droid_media_codec_set_data_callbacks(m_codec, &dcb, &m_out);

    if (!droid_media_codec_start(m_codec)) {
        m_error = "droid_media_codec_start failed";
        droid_media_codec_destroy(m_codec);
        m_codec = nullptr;
        return false;
    }
    return true;
}

bool H264Encoder::queueFrame(uint8_t *frame, size_t size, int64_t ptsUs)
{
    if (!m_codec) {
        free(frame);
        return false;
    }
    FrameCtx *ctx = new FrameCtx{frame};
    DroidMediaCodecData data;
    memset(&data, 0, sizeof(data));
    data.data.data = frame;
    data.data.size = size;
    data.ts = ptsUs;
    data.sync = false;
    DroidMediaBufferCallbacks cb;
    cb.ref = frameRef;
    cb.unref = frameUnref;
    cb.data = ctx;
    droid_media_codec_queue(m_codec, &data, &cb);
    return true;
}

bool H264Encoder::restart()
{
    OutputCallback cb = m_out;
    stop();
    return init(m_width, m_height, m_fps, m_bitrate, cb);
}

bool H264Encoder::restartWith(int width, int height, int bitrate)
{
    OutputCallback cb = m_out;
    stop();
    return init(width, height, m_fps, bitrate, cb);
}

void H264Encoder::setBitrate(int bitrate)
{
    if (m_codec)
        droid_media_codec_set_video_encoder_bitrate(m_codec, bitrate);
}

void H264Encoder::stop()
{
    if (!m_codec)
        return;
    droid_media_codec_stop(m_codec);
    droid_media_codec_destroy(m_codec);
    m_codec = nullptr;
}

} // namespace imira
