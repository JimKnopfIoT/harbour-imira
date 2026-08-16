/*
 * test_tsmux.cpp - standalone smoke test for TsMux / RtpSender.
 * Not part of the daemon build; compile with:
 *   g++ -std=c++14 tsmux.cpp rtpsender.cpp test_tsmux.cpp -o test_tsmux
 */

#include <cstdio>
#include <cstring>
#include <vector>

#include "rtpsender.h"
#include "tsmux.h"

namespace {

bool checkTsAlignment(const std::vector<uint8_t> &out, const char *what) {
    if (out.empty() || out.size() % 188 != 0) {
        std::printf("FAIL: %s: size %zu not a multiple of 188\n", what, out.size());
        return false;
    }
    for (size_t i = 0; i < out.size(); i += 188) {
        if (out[i] != 0x47) {
            std::printf("FAIL: %s: missing sync byte at packet %zu\n", what, i / 188);
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    imira::TsMux mux;

    if (mux.addH264Track(1280, 720, 30, 1) != 0) {
        std::printf("FAIL: addH264Track\n");
        return 1;
    }
    if (mux.addH264Track(1280, 720, 30, 1) != -1) {
        std::printf("FAIL: second addH264Track should be rejected\n");
        return 1;
    }

    // Fake SPS + PPS in Annex-B.
    const uint8_t csd[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x1f, 0xa6, 0x80, 0x50,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
    };
    mux.setCodecConfig(csd, sizeof(csd));

    // Fake IDR access unit in Annex-B, larger than one TS packet.
    std::vector<uint8_t> au;
    const uint8_t idrStart[] = { 0x00, 0x00, 0x00, 0x01, 0x65 };
    au.insert(au.end(), idrStart, idrStart + sizeof(idrStart));
    for (int i = 0; i < 1000; ++i)
        au.push_back(static_cast<uint8_t>(i & 0x7f) | 0x01);

    std::vector<uint8_t> out;
    if (!mux.packetize(au.data(), au.size(), 123456789ll, true, true, out)) {
        std::printf("FAIL: packetize (IDR)\n");
        return 1;
    }
    if (!checkTsAlignment(out, "IDR with PAT/PMT/PCR"))
        return 1;

    // First three packets must be PAT (PID 0), PMT (PID 0x100), PCR (PID 0x1000).
    auto pid = [&](size_t pkt) {
        return ((out[pkt * 188 + 1] & 0x1f) << 8) | out[pkt * 188 + 2];
    };
    if (pid(0) != 0x000 || pid(1) != 0x100 || pid(2) != 0x1000 || pid(3) != 0x1011) {
        std::printf("FAIL: unexpected PID layout %x %x %x %x\n",
                    pid(0), pid(1), pid(2), pid(3));
        return 1;
    }

    // The PES payload of the IDR must start with the prepended SPS.
    // First PES packet: TS header (4) + PES header (14), payload follows.
    const uint8_t *pesPayload = out.data() + 3 * 188 + 4 + 14;
    if (std::memcmp(pesPayload, csd, 8) != 0) {
        std::printf("FAIL: SPS not prepended to IDR frame\n");
        return 1;
    }

    // Non-IDR without tables must contain only video PID packets.
    std::vector<uint8_t> out2;
    const uint8_t nonIdr[] = { 0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x20, 0x10 };
    if (!mux.packetize(nonIdr, sizeof(nonIdr), 123490122ll, false, false, out2)) {
        std::printf("FAIL: packetize (non-IDR)\n");
        return 1;
    }
    if (!checkTsAlignment(out2, "non-IDR"))
        return 1;
    if (out2.size() != 188) {
        std::printf("FAIL: small AU should fit one TS packet, got %zu\n", out2.size());
        return 1;
    }

    // RTP sender: loopback smoke test.
    imira::RtpSender sender;
    if (!sender.open("127.0.0.1", 45990, 0)) {
        std::printf("FAIL: RtpSender::open\n");
        return 1;
    }
    if (!sender.send(out.data(), out.size(), 1000000ll)) {
        std::printf("FAIL: RtpSender::send\n");
        return 1;
    }
    if (sender.localPort() == 0) {
        std::printf("FAIL: RtpSender::localPort\n");
        return 1;
    }

    std::printf("OK: %zu TS packets (IDR), %zu TS packets (non-IDR), RTP local port %u\n",
                out.size() / 188, out2.size() / 188, sender.localPort());
    return 0;
}
