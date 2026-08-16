/*
 * rtpsender.cpp - RTP/UDP sender for MPEG-TS (RFC 2250, payload type 33).
 *
 * Portiert aus aethercast (LGPL-3.0, © Canonical/UBports),
 * src/ac/streaming/rtpsender.cpp and src/ac/network/udpstream.cpp.
 */

#include "rtpsender.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr size_t kRTPHeaderSize = 12;
constexpr size_t kMPEGTSPacketSize = 188;
constexpr uint32_t kSourceID = 0xdeadbeef;
// See http://www.iana.org/assignments/rtp-parameters/rtp-parameters.xhtml
constexpr uint8_t kRTPPayloadTypeMP2T = 33;

// aethercast: 256 KiB transmit buffer, max UDP payload 1472 bytes (below the
// configured MTU so no IP fragmentation is needed).
constexpr int kUdpTxBufferSize = 256 * 1024;
constexpr size_t kMaxUDPPacketSize = 1472;
constexpr size_t kMaxTsPacketsPerRtp =
    (kMaxUDPPacketSize - kRTPHeaderSize) / kMPEGTSPacketSize;  // == 7

} // namespace

namespace imira {

RtpSender::RtpSender() :
    socket_(-1),
    local_port_(0),
    rtp_sequence_number_(0) {
}

RtpSender::~RtpSender() {
    close();
}

void RtpSender::close() {
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
}

bool RtpSender::open(const std::string &destIp, uint16_t destPort, uint16_t localPort) {
    close();

    socket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ < 0)
        return false;

    int value = kUdpTxBufferSize;
    ::setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, &value, sizeof(value));

    struct sockaddr_in addr;
    ::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(localPort);

    if (::bind(socket_, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close();
        return false;
    }

    struct sockaddr_in remote_addr;
    ::memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(destPort);

    if (::inet_pton(AF_INET, destIp.c_str(), &remote_addr.sin_addr) != 1) {
        close();
        return false;
    }

    if (::connect(socket_, reinterpret_cast<const struct sockaddr*>(&remote_addr),
                  sizeof(remote_addr)) < 0) {
        close();
        return false;
    }

    // Determine the actually bound port (relevant when localPort == 0).
    struct sockaddr_in bound_addr;
    socklen_t bound_len = sizeof(bound_addr);
    if (::getsockname(socket_, reinterpret_cast<struct sockaddr*>(&bound_addr),
                      &bound_len) == 0)
        local_port_ = ntohs(bound_addr.sin_port);
    else
        local_port_ = localPort;

    // Non-blocking: a momentarily full send buffer drops packets instead of
    // stalling the pipeline.
    int flags = ::fcntl(socket_, F_GETFL, 0);
    if (flags >= 0)
        ::fcntl(socket_, F_SETFL, flags | O_NONBLOCK);

    rtp_sequence_number_ = 0;

    return true;
}

bool RtpSender::send(const uint8_t *tsData, size_t len, int64_t nowUs) {
    if (socket_ < 0 || !tsData)
        return false;

    // aethercast rejects buffers that are not a whole number of TS packets.
    if (len % kMPEGTSPacketSize != 0)
        return false;

    uint8_t packet[kRTPHeaderSize + kMaxTsPacketsPerRtp * kMPEGTSPacketSize];

    size_t offset = 0;
    while (offset < len) {
        uint8_t *ptr = packet;

        // RTP header: V=2, P=0, X=0, CC=0; M=0, PT=33 (MP2T).
        ptr[0] = 0x80;
        ptr[1] = kRTPPayloadTypeMP2T;

        ptr[2] = (rtp_sequence_number_ >> 8) & 0xff;
        ptr[3] = rtp_sequence_number_ & 0xff;

        rtp_sequence_number_ = (rtp_sequence_number_ + 1) & 0xffff;

        // Adjust time to the 90 kHz RTP clock (same formula as aethercast:
        // wall clock at send time, not the media PTS).
        uint32_t rtp_time = static_cast<uint32_t>((nowUs * 9) / 100ll);

        ptr[4] = rtp_time >> 24;
        ptr[5] = (rtp_time >> 16) & 0xff;
        ptr[6] = (rtp_time >> 8) & 0xff;
        ptr[7] = rtp_time & 0xff;

        ptr[8] = kSourceID >> 24;
        ptr[9] = (kSourceID >> 16) & 0xff;
        ptr[10] = (kSourceID >> 8) & 0xff;
        ptr[11] = kSourceID & 0xff;

        size_t num_ts_packets = (len - offset) / kMPEGTSPacketSize;
        if (num_ts_packets > kMaxTsPacketsPerRtp)
            num_ts_packets = kMaxTsPacketsPerRtp;

        ::memcpy(&ptr[12], tsData + offset, num_ts_packets * kMPEGTSPacketSize);

        const size_t packet_size = kRTPHeaderSize + num_ts_packets * kMPEGTSPacketSize;

        ssize_t bytes_sent = ::send(socket_, packet, packet_size, 0);

        if (bytes_sent < 0) {
            switch (errno) {
            case EAGAIN:
#if EAGAIN != EWOULDBLOCK
            case EWOULDBLOCK:
#endif
                // Send buffer full: drop this RTP packet and keep going.
                break;
            case EINTR:
            case ECONNREFUSED:
            case ENOPROTOOPT:
            case EPROTO:
            case EHOSTUNREACH:
            case ENETUNREACH:
            case ENETDOWN:
                // Possibly transient congestion; retry once like aethercast,
                // then drop on repeated soft failure.
                bytes_sent = ::send(socket_, packet, packet_size, 0);
                if (bytes_sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK
                        && errno != ECONNREFUSED)
                    return false;
                break;
            default:
                return false;
            }
        }

        offset += num_ts_packets * kMPEGTSPacketSize;
    }

    return true;
}

} // namespace imira
