/*
 * rtpsender.h - RTP/UDP sender for MPEG-TS (RFC 2250, payload type 33).
 *
 * Portiert aus aethercast (LGPL-3.0, © Canonical/UBports),
 * src/ac/streaming/rtpsender.cpp and src/ac/network/udpstream.cpp,
 * simplified: no worker thread/queue, packets are sent directly via
 * sendto on a non-blocking UDP socket.
 */

#ifndef IMIRA_RTPSENDER_H_
#define IMIRA_RTPSENDER_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace imira {

// Wraps MPEG-TS packets into RTP packets (up to 7 x 188-byte TS packets per
// RTP packet, fitting a 1472-byte UDP payload below the usual MTU) and sends
// them over a connected, non-blocking UDP socket.
class RtpSender {
public:
    RtpSender();
    ~RtpSender();

    RtpSender(const RtpSender &) = delete;
    RtpSender &operator=(const RtpSender &) = delete;

    // Creates the UDP socket, sets SO_SNDBUF (256 KiB, as aethercast),
    // binds it to localPort (0 picks an ephemeral port) and connects it to
    // destIp:destPort. destIp must be a numeric IPv4 address.
    bool open(const std::string &destIp, uint16_t destPort, uint16_t localPort);

    // Sends tsData (len must be a multiple of 188) as one or more RTP
    // packets. nowUs is the current time in microseconds; the RTP timestamp
    // is derived from it on a 90 kHz base. Packets that would block (full
    // send buffer) are dropped, matching UDP streaming semantics. Returns
    // false on hard socket errors or invalid input.
    bool send(const uint8_t *tsData, size_t len, int64_t nowUs);

    void close();

    // Actual local port after open() (useful for RTSP negotiation).
    uint16_t localPort() const { return local_port_; }

private:
    int socket_;
    uint16_t local_port_;
    uint16_t rtp_sequence_number_;
};

} // namespace imira

#endif // IMIRA_RTPSENDER_H_
