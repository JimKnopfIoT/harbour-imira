/*
 * tsmux.h - MPEG-TS packetizer for Wifi-Display (Miracast) H.264 streaming.
 *
 * Portiert aus aethercast (LGPL-3.0, © Canonical/UBports),
 * src/ac/streaming/mpegtspacketizer.{h,cpp}, which itself is based on
 * Android's TSPacketizer (Copyright 2012, The Android Open Source Project,
 * Apache-2.0). Stripped of all aethercast/ac dependencies; C++14 stdlib only.
 *
 * The WFD LPCM audio support (stream type 0x83, private_stream_1, LPCM
 * sub header, access unit chunking, s16le->s16be conversion) is ported from
 * Android's frameworks/av/media/libstagefright/wifi-display/source/
 * {TSPacketizer,Converter}.cpp (Copyright 2012, The Android Open Source
 * Project, Apache-2.0).
 */

#ifndef IMIRA_TSMUX_H_
#define IMIRA_TSMUX_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace imira {

// Packetizes H.264 access units into 188-byte MPEG-TS packets following the
// WiFi Display spec (PMT PID 0x100, PCR PID 0x1000, video PID 0x1011,
// stream type 0x1b, stream id 0xe0, PTS-only PES headers on a 90 kHz base).
//
// Optionally adds one WFD LPCM audio track (PID 0x1100, stream type 0x83,
// PES private_stream_1 id 0xbd, 4-byte LPCM sub header per access unit).
//
// Access units MUST be in Annex-B format (with 00 00 00 01 / 00 00 01 start
// codes), exactly as aethercast expects them from the encoder.
class TsMux {
public:
    TsMux();

    // Adds the single H.264 video track. Returns the track id (always 0) or
    // -1 if a track has already been added. width/height/fps are accepted for
    // API completeness; the TS layer itself does not embed them (matching
    // aethercast, where the PMT descriptors carry only profile/level info).
    int addH264Track(int width, int height, int fpsNum, int fpsDen);

    // Submits codec specific data (SPS/PPS) as one Annex-B blob. The
    // contained NAL units are split and stored; they are automatically
    // prepended to IDR access units in packetize() (aethercast's
    // SubmitCSD/PrependCSD mechanics) and the SPS bytes feed the AVC video
    // descriptor in the PMT.
    void setCodecConfig(const uint8_t *csd, size_t len);

    // Packetizes one access unit. ptsUs is the presentation time in
    // microseconds (converted to the 90 kHz PTS base internally). If isIdr is
    // true and codec config was submitted, SPS/PPS are prepended to the
    // payload. If withPcrAndPatPmt is true, PAT, PMT and a PCR packet are
    // emitted in front of the PES packets (callers should set this at least
    // every 100 ms, per spec). "out" is cleared and filled with a whole
    // number of 188-byte TS packets. Returns false if no track was added.
    bool packetize(const uint8_t *accessUnit, size_t len, int64_t ptsUs,
                   bool isIdr, bool withPcrAndPatPmt,
                   std::vector<uint8_t> &out);

    // Adds the single WFD LPCM audio track. Only 48000 Hz / 2 channels
    // (16-bit) is supported; returns the track id (always 1) or -1 if the
    // mode is unsupported or an audio track has already been added. Once
    // added, the PMT emitted by packetize() advertises the track (stream
    // type 0x83 with the LPCM audio stream descriptor). Add the track before
    // streaming starts: the PAT/PMT version number is static, so receivers
    // are not required to pick up mid-stream PMT changes.
    int addLpcmTrack(int sampleRate, int channels);

    // Packetizes S16LE interleaved stereo PCM. ptsUs is the presentation
    // time of the FIRST sample in "pcm", in microseconds (90 kHz PTS base
    // internally, like video). len must be a multiple of 4 (one stereo
    // frame). Following Android's Converter::feedRawAudioInputBuffers, the
    // samples are converted to big endian and chunked into access units of
    // 6 * 80 stereo frames (1920 bytes == 10 ms), each prepended with the
    // 4-byte WFD LPCM sub header and emitted as its own PES packet on
    // private_stream_1. A trailing partial access unit is buffered
    // internally and emitted on a later call; callers are expected to
    // deliver a gapless stream (a discontinuous ptsUs only takes effect at
    // the next access unit boundary). "out" is cleared and filled with a
    // whole number of 188-byte TS packets (possibly zero if all input was
    // buffered). PAT/PMT/PCR are never emitted here; they piggyback on the
    // video packetize() calls. Returns false if no audio track was added or
    // the input is invalid.
    bool packetizeAudio(const uint8_t *pcm, size_t len, int64_t ptsUs,
                        std::vector<uint8_t> &out);

private:
    void initCrcTable();
    uint32_t calcCrc32(const uint8_t *start, size_t size) const;
    void finalizeTrack();
    unsigned int nextContinuityCounter();
    unsigned int nextAudioContinuityCounter();
    void emitPatAndPmt(uint8_t *packetDataStart);
    void emitPcr(uint8_t *packetDataStart);
    // Appends the TS packets of one audio PES packet (LPCM sub header +
    // payload already assembled in "au") to "out". pts90 is on the 90 kHz
    // base.
    void appendAudioPes(const uint8_t *au, size_t auLen, uint64_t pts90,
                        std::vector<uint8_t> &out);

    bool have_track_;
    unsigned int pat_continuity_counter_;
    unsigned int pmt_continuity_counter_;
    unsigned int track_continuity_counter_;
    bool track_finalized_;
    uint32_t crc_table_[256];
    // Each entry is one NAL unit prefixed with a 4-byte Annex-B start code.
    std::vector<std::vector<uint8_t>> csd_;
    // PMT ES descriptors (AVC video descriptor + AVC timing and HRD).
    std::vector<std::vector<uint8_t>> descriptors_;

    bool have_audio_track_;
    unsigned int audio_continuity_counter_;
    // PMT ES descriptor for the audio track (LPCM audio stream descriptor).
    std::vector<uint8_t> audio_descriptor_;
    // Big-endian PCM bytes not yet forming a whole access unit, plus the
    // presentation time (µs) of its first sample.
    std::vector<uint8_t> audio_pending_;
    int64_t audio_pending_pts_us_;
};

} // namespace imira

#endif // IMIRA_TSMUX_H_
