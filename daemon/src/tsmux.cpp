/*
 * tsmux.cpp - MPEG-TS packetizer for Wifi-Display (Miracast) H.264 streaming.
 *
 * Portiert aus aethercast (LGPL-3.0, © Canonical/UBports),
 * src/ac/streaming/mpegtspacketizer.cpp and src/ac/video/utils_from_android.cpp,
 * which are based on Android's TSPacketizer / avc_utils
 * (Copyright 2012, The Android Open Source Project, Apache-2.0).
 *
 * The WFD LPCM audio support is ported from Android's
 * frameworks/av/media/libstagefright/wifi-display/source/TSPacketizer.cpp
 * (addTrack: stream type 0x83, private_stream_1, PID 0x1100; finalize: LPCM
 * audio stream descriptor) and Converter.cpp (feedRawAudioInputBuffers:
 * access unit chunking, LPCM sub header, s16le->s16be conversion), both
 * Copyright 2012, The Android Open Source Project, Apache-2.0.
 */

#include "tsmux.h"

#include <cstring>
#include <ctime>

namespace {

const uint8_t kH264NALPrefix[] = { 0x00, 0x00, 0x00, 0x01 };

// One byte each for profile, constraint set and level.
constexpr size_t kCSDSize = 3;

// See WiFi Display spec version 1.1 chapter D.4.2 PAT/PMT.
constexpr unsigned int kPIDofPMT = 0x100;
constexpr unsigned int kPIDofPCR = 0x1000;
constexpr unsigned int kVideoPID = 0x1011;

constexpr unsigned int kH264StreamType = 0x1b;
constexpr unsigned int kVideoStreamId = 0xe0;
constexpr unsigned int kAVCVideoDescriptorTag = 40;
constexpr unsigned int kAVCTimingAndHRDDescriptorTag = 42;

// WFD LPCM audio track, values from Android's TSPacketizer::addTrack():
// first audio PID 0x1100, stream_type 0x83, stream id private_stream_1.
constexpr unsigned int kAudioPID = 0x1100;
constexpr unsigned int kLpcmStreamType = 0x83;
constexpr unsigned int kLpcmStreamId = 0xbd;
// LPCM audio stream descriptor uses the stream type value as its tag.
constexpr unsigned int kLpcmDescriptorTag = 0x83;

// Access unit layout from Android's Converter::feedRawAudioInputBuffers():
// "Split incoming PCM audio into buffers of 6 AUs of 80 audio frames each
//  and add a 4 byte header according to the wifi display specs."
constexpr size_t kLpcmFrameSize = 2 * sizeof(int16_t);  // stereo
constexpr size_t kLpcmFramesPerAU = 80;
constexpr size_t kLpcmNumAUsPerPESPacket = 6;
constexpr size_t kLpcmPcmBytesPerAccessUnit =
    kLpcmNumAUsPerPESPacket * kLpcmFramesPerAU * kLpcmFrameSize;  // 1920
constexpr size_t kLpcmSubHeaderSize = 4;
constexpr int kLpcmSampleRate = 48000;

// Fallback values for the AVC video descriptor when no codec config was
// submitted: Constrained Baseline profile, level 3.1.
constexpr uint8_t kDefaultProfileIdc = 66;
constexpr uint8_t kDefaultConstraintSet = 0xc0;
constexpr uint8_t kDefaultLevelIdc = 31;

int64_t GetNowUs() {
    struct timespec ts;
    ::memset(&ts, 0, sizeof(ts));
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000ll + ts.tv_nsec / 1000ll;
}

// Annex-B NAL unit scanner, ported verbatim from Android's avc_utils
// (as vendored by aethercast in utils_from_android.cpp).
bool GetNextNALUnit(const uint8_t **_data, size_t *_size,
                    const uint8_t **nalStart, size_t *nalSize,
                    bool startCodeFollows) {
    const uint8_t *data = *_data;
    size_t size = *_size;

    *nalStart = nullptr;
    *nalSize = 0;

    if (size < 3)
        return false;

    size_t offset = 0;

    // A valid startcode consists of at least two 0x00 bytes followed by 0x01.
    for (; offset + 2 < size; ++offset) {
        if (data[offset + 2] == 0x01 && data[offset] == 0x00
                && data[offset + 1] == 0x00)
            break;
    }
    if (offset + 2 >= size) {
        *_data = &data[offset];
        *_size = 2;
        return false;
    }
    offset += 3;

    size_t startOffset = offset;

    for (;;) {
        while (offset < size && data[offset] != 0x01)
            ++offset;

        if (offset == size) {
            if (startCodeFollows) {
                offset = size + 2;
                break;
            }
            return false;
        }

        if (data[offset - 1] == 0x00 && data[offset - 2] == 0x00)
            break;

        ++offset;
    }

    size_t endOffset = offset - 2;
    while (endOffset > startOffset + 1 && data[endOffset - 1] == 0x00)
        --endOffset;

    *nalStart = &data[startOffset];
    *nalSize = endOffset - startOffset;

    if (offset + 2 < size) {
        *_data = &data[offset - 2];
        *_size = size - offset + 2;
    } else {
        *_data = nullptr;
        *_size = 0;
    }

    return true;
}

} // namespace

namespace imira {

TsMux::TsMux() :
    have_track_(false),
    pat_continuity_counter_(0),
    pmt_continuity_counter_(0),
    track_continuity_counter_(0),
    track_finalized_(false),
    have_audio_track_(false),
    audio_continuity_counter_(0),
    audio_pending_pts_us_(0) {
    initCrcTable();
}

int TsMux::addH264Track(int width, int height, int fpsNum, int fpsDen) {
    // The TS layer does not embed resolution or frame rate (aethercast's PMT
    // descriptors advertise fixed_frame_rate_flag=0 and no timing info).
    (void)width;
    (void)height;
    (void)fpsNum;
    (void)fpsDen;

    if (have_track_)
        return -1;

    have_track_ = true;
    track_continuity_counter_ = 0;
    return 0;
}

int TsMux::addLpcmTrack(int sampleRate, int channels) {
    if (have_audio_track_)
        return -1;

    // Android's Track::finalize() also allows 44100 Hz, but WFD LPCM in
    // this project only needs 48 kHz stereo 16-bit.
    if (sampleRate != kLpcmSampleRate || channels != 2)
        return -1;

    // LPCM audio stream descriptor, built exactly like Android's
    // TSPacketizer Track::finalize() for "audio/raw":
    audio_descriptor_.resize(4);
    audio_descriptor_[0] = kLpcmDescriptorTag;  // descriptor_tag
    audio_descriptor_[1] = 2;                   // descriptor_length

    const unsigned sampling_frequency = (sampleRate == 44100) ? 1 : 2;
    audio_descriptor_[2] = (sampling_frequency << 5)
            | (3 /* reserved */ << 1)
            | 0 /* emphasis_flag */;
    audio_descriptor_[3] = (1 /* number_of_channels = stereo */ << 5)
            | 0xf /* reserved */;

    have_audio_track_ = true;
    audio_continuity_counter_ = 0;
    audio_pending_.clear();
    audio_pending_pts_us_ = 0;
    return 1;
}

void TsMux::setCodecConfig(const uint8_t *csd, size_t len) {
    csd_.clear();
    descriptors_.clear();
    track_finalized_ = false;

    const uint8_t *data = csd;
    size_t size = len;
    const uint8_t *nal_start;
    size_t nal_size;

    while (GetNextNALUnit(&data, &size, &nal_start, &nal_size, true)) {
        std::vector<uint8_t> current(nal_size + sizeof(kH264NALPrefix));
        ::memcpy(current.data(), kH264NALPrefix, sizeof(kH264NALPrefix));
        ::memcpy(current.data() + sizeof(kH264NALPrefix), nal_start, nal_size);
        csd_.push_back(std::move(current));
    }
}

void TsMux::finalizeTrack() {
    if (track_finalized_)
        return;

    {
        // AVC video descriptor (40)
        std::vector<uint8_t> descriptor(6);
        uint8_t *data = descriptor.data();
        data[0] = kAVCVideoDescriptorTag;  // descriptor_tag
        data[1] = 4;                       // descriptor_length

        if (!csd_.empty() && csd_.at(0).size() >= sizeof(kH264NALPrefix) + 1 + kCSDSize) {
            // By convention the first submitted NAL is the SPS. Skip the
            // 4-byte start code and the NAL header byte, then copy
            // profile/constraint/level (one byte each).
            //
            // Note: aethercast copies starting right after the start code,
            // i.e. beginning with the NAL header byte (0x67) instead of the
            // profile_idc. We skip the NAL header byte so the descriptor
            // carries the actual profile/constraint/level bytes.
            ::memcpy(&data[2], csd_.at(0).data() + sizeof(kH264NALPrefix) + 1, kCSDSize);
        } else {
            data[2] = kDefaultProfileIdc;
            data[3] = kDefaultConstraintSet;
            data[4] = kDefaultLevelIdc;
        }

        // AVC_still_present=0, AVC_24_hour_picture_flag=0, reserved
        data[5] = 0x3f;

        descriptors_.push_back(std::move(descriptor));
    }
    {
        // AVC timing and HRD descriptor (42)
        std::vector<uint8_t> descriptor(4);
        uint8_t *data = descriptor.data();
        data[0] = kAVCTimingAndHRDDescriptorTag;  // descriptor_tag
        data[1] = 2;                              // descriptor_length

        // hrd_management_valid_flag=0, reserved=111111b,
        // picture_and_timing_info_present=0
        data[2] = 0x7e;

        // fixed_frame_rate_flag=0, temporal_poc_flag=0,
        // picture_to_display_conversion_flag=0, reserved=11111b
        data[3] = 0x1f;

        descriptors_.push_back(std::move(descriptor));
    }

    track_finalized_ = true;
}

unsigned int TsMux::nextContinuityCounter() {
    unsigned int prev = track_continuity_counter_;
    if (++track_continuity_counter_ == 16)
        track_continuity_counter_ = 0;
    return prev;
}

unsigned int TsMux::nextAudioContinuityCounter() {
    unsigned int prev = audio_continuity_counter_;
    if (++audio_continuity_counter_ == 16)
        audio_continuity_counter_ = 0;
    return prev;
}

void TsMux::emitPatAndPmt(uint8_t *packetDataStart) {
    // Program Association Table (PAT):
    // 0x47
    // transport_error_indicator = b0
    // payload_unit_start_indicator = b1
    // transport_priority = b0
    // PID = b0000000000000 (13 bits)
    // transport_scrambling_control = b00
    // adaptation_field_control = b01 (no adaptation field, payload only)
    // continuity_counter
    // pointer_field = 0x00
    // --- payload: table_id 0x00, one program (0x0001) -> PMT PID, CRC

    if (++pat_continuity_counter_ == 16)
        pat_continuity_counter_ = 0;

    uint8_t *ptr = packetDataStart;
    *ptr++ = 0x47;
    *ptr++ = 0x40;
    *ptr++ = 0x00;
    *ptr++ = 0x10 | pat_continuity_counter_;
    *ptr++ = 0x00;

    uint8_t *crcDataStart = ptr;
    *ptr++ = 0x00;
    *ptr++ = 0xb0;
    *ptr++ = 0x0d;
    *ptr++ = 0x00;
    *ptr++ = 0x00;
    *ptr++ = 0xc3;
    *ptr++ = 0x00;
    *ptr++ = 0x00;
    *ptr++ = 0x00;
    *ptr++ = 0x01;
    *ptr++ = 0xe0 | (kPIDofPMT >> 8);
    *ptr++ = kPIDofPMT & 0xff;

    uint32_t crc = calcCrc32(crcDataStart, ptr - crcDataStart);
    *ptr++ = (crc >> 24) & 0xff;
    *ptr++ = (crc >> 16) & 0xff;
    *ptr++ = (crc >> 8) & 0xff;
    *ptr++ = crc & 0xff;

    ::memset(ptr, 0xff, packetDataStart + 188 - ptr);

    packetDataStart += 188;

    // Program Map Table (PMT):
    // 0x47, PUSI=1, PID = kPIDofPMT, payload only, continuity_counter
    // pointer_field, table_id 0x02, program 0x0001, PCR PID,
    // program_info, per-stream (stream_type, PID, ES descriptors), CRC

    if (++pmt_continuity_counter_ == 16)
        pmt_continuity_counter_ = 0;

    ptr = packetDataStart;
    *ptr++ = 0x47;
    *ptr++ = 0x40 | (kPIDofPMT >> 8);
    *ptr++ = kPIDofPMT & 0xff;
    *ptr++ = 0x10 | pmt_continuity_counter_;
    *ptr++ = 0x00;

    crcDataStart = ptr;
    *ptr++ = 0x02;

    *ptr++ = 0x00;  // section_length, filled in below
    *ptr++ = 0x00;

    *ptr++ = 0x00;
    *ptr++ = 0x01;
    *ptr++ = 0xc3;
    *ptr++ = 0x00;
    *ptr++ = 0x00;
    *ptr++ = 0xe0 | (kPIDofPCR >> 8);
    *ptr++ = kPIDofPCR & 0xff;

    // program_info_length = 0 (no program info descriptors)
    *ptr++ = 0xf0;
    *ptr++ = 0x00;

    // Make sure all descriptors have been built.
    finalizeTrack();

    *ptr++ = kH264StreamType;
    *ptr++ = 0xe0 | (kVideoPID >> 8);
    *ptr++ = kVideoPID & 0xff;

    size_t ES_info_length = 0;
    for (const auto &descriptor : descriptors_)
        ES_info_length += descriptor.size();

    *ptr++ = 0xf0 | (ES_info_length >> 8);
    *ptr++ = ES_info_length & 0xff;

    for (const auto &descriptor : descriptors_) {
        ::memcpy(ptr, descriptor.data(), descriptor.size());
        ptr += descriptor.size();
    }

    if (have_audio_track_) {
        *ptr++ = kLpcmStreamType;
        *ptr++ = 0xe0 | (kAudioPID >> 8);
        *ptr++ = kAudioPID & 0xff;

        *ptr++ = 0xf0 | (audio_descriptor_.size() >> 8);
        *ptr++ = audio_descriptor_.size() & 0xff;

        ::memcpy(ptr, audio_descriptor_.data(), audio_descriptor_.size());
        ptr += audio_descriptor_.size();
    }

    size_t section_length = ptr - (crcDataStart + 3) + 4 /* CRC */;

    crcDataStart[1] = 0xb0 | (section_length >> 8);
    crcDataStart[2] = section_length & 0xff;

    crc = calcCrc32(crcDataStart, ptr - crcDataStart);
    *ptr++ = (crc >> 24) & 0xff;
    *ptr++ = (crc >> 16) & 0xff;
    *ptr++ = (crc >> 8) & 0xff;
    *ptr++ = crc & 0xff;

    ::memset(ptr, 0xff, packetDataStart + 188 - ptr);
}

void TsMux::emitPcr(uint8_t *packetDataStart) {
    // PCR-only packet: PID = kPIDofPCR, adaptation field only (no payload),
    // continuity counter fixed to 0 (does not increment for AF-only packets).
    // PCR is based on a 27 MHz clock derived from the monotonic clock.
    int64_t nowUs = GetNowUs();
    uint64_t PCR = static_cast<uint64_t>(nowUs) * 27;
    uint64_t PCR_base = PCR / 300;
    uint32_t PCR_ext = PCR % 300;

    uint8_t *ptr = packetDataStart;
    *ptr++ = 0x47;
    *ptr++ = 0x40 | (kPIDofPCR >> 8);
    *ptr++ = kPIDofPCR & 0xff;
    *ptr++ = 0x20;
    *ptr++ = 0xb7;  // adaptation_field_length
    // Note: aethercast's copy of this code dropped the "(PCR_base >> 1)"
    // byte, emitting a malformed 5-byte PCR field. We restore the correct
    // 6-byte encoding (33-bit base, 6 reserved bits, 9-bit extension) as in
    // Android's original TSPacketizer.
    *ptr++ = 0x10;  // PCR_flag = 1
    *ptr++ = (PCR_base >> 25) & 0xff;
    *ptr++ = (PCR_base >> 17) & 0xff;
    *ptr++ = (PCR_base >> 9) & 0xff;
    *ptr++ = (PCR_base >> 1) & 0xff;
    *ptr++ = ((PCR_base & 1) << 7) | 0x7e | ((PCR_ext >> 8) & 1);
    *ptr++ = PCR_ext & 0xff;

    ::memset(ptr, 0xff, packetDataStart + 188 - ptr);
}

bool TsMux::packetize(const uint8_t *accessUnit, size_t len, int64_t ptsUs,
                      bool isIdr, bool withPcrAndPatPmt,
                      std::vector<uint8_t> &out) {
    out.clear();

    if (!have_track_ || !accessUnit || len == 0)
        return false;

    // Prepend codec specific data (SPS and PPS) to IDR frames, matching
    // aethercast's kPrependSPSandPPStoIDRFrames + PrependCSD behavior.
    std::vector<uint8_t> prepended;
    const uint8_t *auData = accessUnit;
    size_t auLen = len;

    if (isIdr && !csd_.empty()) {
        size_t csdSize = 0;
        for (const auto &current : csd_)
            csdSize += current.size();

        prepended.resize(csdSize + len);
        size_t offset = 0;
        for (const auto &current : csd_) {
            ::memcpy(prepended.data() + offset, current.data(), current.size());
            offset += current.size();
        }
        ::memcpy(prepended.data() + offset, accessUnit, len);

        auData = prepended.data();
        auLen = prepended.size();
    }

    // PES layout (first TS packet):
    //   4 bytes TS header, optional adaptation field stuffing,
    //   14 bytes static PES header (start code, stream_id,
    //   PES_packet_length, flags, PES_header_data_length, 5-byte PTS),
    //   then payload. Subsequent TS packets: 4 bytes TS header + payload.
    size_t PES_packet_length = auLen + 8;

    size_t numTSPackets = 1;
    {
        constexpr size_t PES_header_size = 14;
        size_t sizeAvailableForPayload = 188 - 4 - PES_header_size;
        size_t numBytesOfPayload = auLen;

        if (numBytesOfPayload > sizeAvailableForPayload)
            numBytesOfPayload = sizeAvailableForPayload;

        size_t numBytesOfPayloadRemaining = auLen - numBytesOfPayload;

        // Maximum payload per subsequent TS packet.
        sizeAvailableForPayload = 188 - 4;

        size_t numFullTSPackets =
            numBytesOfPayloadRemaining / sizeAvailableForPayload;

        numTSPackets += numFullTSPackets;

        numBytesOfPayloadRemaining -=
            numFullTSPackets * sizeAvailableForPayload;

        if (numBytesOfPayloadRemaining > 0)
            ++numTSPackets;
    }

    if (withPcrAndPatPmt)
        numTSPackets += 3;  // PAT + PMT + PCR

    out.resize(numTSPackets * 188);
    uint8_t *packetDataStart = out.data();

    if (withPcrAndPatPmt) {
        emitPatAndPmt(packetDataStart);
        packetDataStart += 2 * 188;
        emitPcr(packetDataStart);
        packetDataStart += 188;
    }

    // Adjust time to the 90 kHz PTS base.
    uint64_t PTS = (static_cast<uint64_t>(ptsUs) * 9ll) / 100ll;

    if (PES_packet_length >= 65536) {
        // Valid for video per spec: 0 means unbounded.
        PES_packet_length = 0;
    }

    size_t sizeAvailableForPayload = 188 - 4 - 14;
    size_t copy = auLen;

    if (copy > sizeAvailableForPayload)
        copy = sizeAvailableForPayload;

    size_t numPaddingBytes = sizeAvailableForPayload - copy;

    // First TS packet of the PES packet:
    // 0x47, PUSI=1, PID = video PID, adaptation field iff padding needed,
    // continuity counter; then the PES header:
    //   packet_startcode_prefix 0x000001, stream_id,
    //   PES_packet_length, '10' + data_alignment_indicator (0x84),
    //   PTS_DTS_flags = b10 (PTS only, 0x80), PES_header_data_length = 5,
    //   5-byte PTS with marker bits.
    uint8_t *ptr = packetDataStart;
    *ptr++ = 0x47;
    *ptr++ = 0x40 | (kVideoPID >> 8);
    *ptr++ = kVideoPID & 0xff;

    *ptr++ = (numPaddingBytes > 0 ? 0x30 : 0x10) | nextContinuityCounter();

    if (numPaddingBytes > 0) {
        *ptr++ = numPaddingBytes - 1;
        if (numPaddingBytes >= 2) {
            *ptr++ = 0x00;
            ::memset(ptr, 0xff, numPaddingBytes - 2);
            ptr += numPaddingBytes - 2;
        }
    }

    *ptr++ = 0x00;
    *ptr++ = 0x00;
    *ptr++ = 0x01;
    *ptr++ = kVideoStreamId;
    *ptr++ = PES_packet_length >> 8;
    *ptr++ = PES_packet_length & 0xff;
    *ptr++ = 0x84;
    *ptr++ = 0x80;
    *ptr++ = 0x05;  // PES_header_data_length

    *ptr++ = 0x20 | (((PTS >> 30) & 7) << 1) | 1;
    *ptr++ = (PTS >> 22) & 0xff;
    *ptr++ = (((PTS >> 15) & 0x7f) << 1) | 1;
    *ptr++ = (PTS >> 7) & 0xff;
    *ptr++ = ((PTS & 0x7f) << 1) | 1;

    ::memcpy(ptr, auData, copy);
    ptr += copy;

    packetDataStart += 188;

    size_t offset = copy;
    while (offset < auLen) {
        // Subsequent fragments: 0x47, PUSI=0, video PID, adaptation field
        // stuffing iff the remaining payload doesn't fill the packet,
        // continuity counter, payload.
        size_t sizeAvailable = 188 - 4;

        size_t chunk = auLen - offset;
        if (chunk > sizeAvailable)
            chunk = sizeAvailable;

        size_t padding = sizeAvailable - chunk;

        ptr = packetDataStart;
        *ptr++ = 0x47;
        *ptr++ = 0x00 | (kVideoPID >> 8);
        *ptr++ = kVideoPID & 0xff;

        *ptr++ = (padding > 0 ? 0x30 : 0x10) | nextContinuityCounter();

        if (padding > 0) {
            *ptr++ = padding - 1;
            if (padding >= 2) {
                *ptr++ = 0x00;
                ::memset(ptr, 0xff, padding - 2);
                ptr += padding - 2;
            }
        }

        ::memcpy(ptr, auData + offset, chunk);
        ptr += chunk;

        offset += chunk;
        packetDataStart += 188;
    }

    return true;
}

bool TsMux::packetizeAudio(const uint8_t *pcm, size_t len, int64_t ptsUs,
                           std::vector<uint8_t> &out) {
    out.clear();

    if (!have_audio_track_ || !pcm || len == 0 || (len % kLpcmFrameSize) != 0)
        return false;

    // If nothing is buffered the incoming timestamp becomes the timestamp of
    // the next access unit. Otherwise the pending data determines it (the
    // stream is assumed to be gapless, as in Android's Converter which also
    // derives continuation times purely from the sample count).
    if (audio_pending_.empty())
        audio_pending_pts_us_ = ptsUs;

    // s16le -> s16be: WFD/Blu-ray LPCM payload is big endian. Android does
    // "*ptr = htons(*ptr)" over the whole buffer in
    // Converter::feedRawAudioInputBuffers(); since the input here is defined
    // as S16LE the swap is unconditional.
    size_t oldSize = audio_pending_.size();
    audio_pending_.resize(oldSize + len);
    for (size_t i = 0; i < len; i += 2) {
        audio_pending_[oldSize + i] = pcm[i + 1];
        audio_pending_[oldSize + i + 1] = pcm[i];
    }

    size_t consumed = 0;
    while (audio_pending_.size() - consumed >= kLpcmPcmBytesPerAccessUnit) {
        // One access unit: 4-byte LPCM sub header + 6 * 80 stereo frames,
        // exactly as built in Android's Converter::feedRawAudioInputBuffers:
        //   ptr[0] = 0xa0;  // 10100000b (sub_stream_id)
        //   ptr[1] = kNumAUsPerPESPacket;  // number_of_frame_headers = 6
        //   ptr[2] = 0;  // reserved, audio _emphasis_flag = 0
        //   ptr[3] = (quantization_word_length=0 /*16-bit*/ << 6)
        //          | (audio_sampling_frequency=2 /*48kHz*/ << 3)
        //          | (number_of_audio_channels=1 /*stereo*/);
        uint8_t au[kLpcmSubHeaderSize + kLpcmPcmBytesPerAccessUnit];
        au[0] = 0xa0;
        au[1] = kLpcmNumAUsPerPESPacket;
        au[2] = 0;
        au[3] = (0 << 6) | (2 << 3) | 1;
        ::memcpy(au + kLpcmSubHeaderSize, audio_pending_.data() + consumed,
                 kLpcmPcmBytesPerAccessUnit);

        // Adjust time to the 90 kHz PTS base (like video).
        uint64_t PTS =
            (static_cast<uint64_t>(audio_pending_pts_us_) * 9ll) / 100ll;

        appendAudioPes(au, sizeof(au), PTS, out);

        consumed += kLpcmPcmBytesPerAccessUnit;
        // 480 stereo frames @ 48 kHz = exactly 10 ms.
        audio_pending_pts_us_ +=
            (kLpcmPcmBytesPerAccessUnit / kLpcmFrameSize) * 1000000ll
                / kLpcmSampleRate;
    }

    if (consumed > 0)
        audio_pending_.erase(audio_pending_.begin(),
                             audio_pending_.begin() + consumed);

    return true;
}

void TsMux::appendAudioPes(const uint8_t *au, size_t auLen, uint64_t pts90,
                           std::vector<uint8_t> &out) {
    // Same PES/TS layout as the video path, but on the audio PID with
    // stream id private_stream_1 (0xbd). PES_packet_length always fits for
    // LPCM access units (1924 + 8 bytes), so it is never reset to 0.
    size_t PES_packet_length = auLen + 8;

    constexpr size_t firstPacketPayload = 188 - 4 - 14;

    size_t numTSPackets = 1;
    {
        size_t remaining =
            auLen > firstPacketPayload ? auLen - firstPacketPayload : 0;
        numTSPackets += remaining / (188 - 4);
        if (remaining % (188 - 4))
            ++numTSPackets;
    }

    size_t outOffset = out.size();
    out.resize(outOffset + numTSPackets * 188);
    uint8_t *packetDataStart = out.data() + outOffset;

    size_t copy = auLen;
    if (copy > firstPacketPayload)
        copy = firstPacketPayload;

    size_t numPaddingBytes = firstPacketPayload - copy;

    uint8_t *ptr = packetDataStart;
    *ptr++ = 0x47;
    *ptr++ = 0x40 | (kAudioPID >> 8);
    *ptr++ = kAudioPID & 0xff;

    *ptr++ = (numPaddingBytes > 0 ? 0x30 : 0x10)
            | nextAudioContinuityCounter();

    if (numPaddingBytes > 0) {
        *ptr++ = numPaddingBytes - 1;
        if (numPaddingBytes >= 2) {
            *ptr++ = 0x00;
            ::memset(ptr, 0xff, numPaddingBytes - 2);
            ptr += numPaddingBytes - 2;
        }
    }

    *ptr++ = 0x00;
    *ptr++ = 0x00;
    *ptr++ = 0x01;
    *ptr++ = kLpcmStreamId;
    *ptr++ = PES_packet_length >> 8;
    *ptr++ = PES_packet_length & 0xff;
    *ptr++ = 0x84;
    *ptr++ = 0x80;
    *ptr++ = 0x05;  // PES_header_data_length

    *ptr++ = 0x20 | (((pts90 >> 30) & 7) << 1) | 1;
    *ptr++ = (pts90 >> 22) & 0xff;
    *ptr++ = (((pts90 >> 15) & 0x7f) << 1) | 1;
    *ptr++ = (pts90 >> 7) & 0xff;
    *ptr++ = ((pts90 & 0x7f) << 1) | 1;

    ::memcpy(ptr, au, copy);
    ptr += copy;

    packetDataStart += 188;

    size_t offset = copy;
    while (offset < auLen) {
        size_t sizeAvailable = 188 - 4;

        size_t chunk = auLen - offset;
        if (chunk > sizeAvailable)
            chunk = sizeAvailable;

        size_t padding = sizeAvailable - chunk;

        ptr = packetDataStart;
        *ptr++ = 0x47;
        *ptr++ = 0x00 | (kAudioPID >> 8);
        *ptr++ = kAudioPID & 0xff;

        *ptr++ = (padding > 0 ? 0x30 : 0x10) | nextAudioContinuityCounter();

        if (padding > 0) {
            *ptr++ = padding - 1;
            if (padding >= 2) {
                *ptr++ = 0x00;
                ::memset(ptr, 0xff, padding - 2);
                ptr += padding - 2;
            }
        }

        ::memcpy(ptr, au + offset, chunk);
        ptr += chunk;

        offset += chunk;
        packetDataStart += 188;
    }
}

void TsMux::initCrcTable() {
    // MPEG-2 CRC32 (poly 0x04C11DB7, MSB-first, init 0xffffffff, no final xor)
    const uint32_t poly = 0x04C11DB7;

    for (int i = 0; i < 256; i++) {
        uint32_t crc = static_cast<uint32_t>(i) << 24;
        for (int j = 0; j < 8; j++)
            crc = (crc << 1) ^ ((crc & 0x80000000) ? poly : 0);
        crc_table_[i] = crc;
    }
}

uint32_t TsMux::calcCrc32(const uint8_t *start, size_t size) const {
    uint32_t crc = 0xFFFFFFFF;

    for (const uint8_t *p = start; p < start + size; ++p)
        crc = (crc << 8) ^ crc_table_[((crc >> 24) ^ *p) & 0xFF];

    return crc;
}

} // namespace imira
