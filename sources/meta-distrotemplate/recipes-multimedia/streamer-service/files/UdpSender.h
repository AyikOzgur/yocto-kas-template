#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

/// RTP or MPEG-TS over plain UDP, e.g. to wfb_tx on the same board. No feedback and no
/// retransmissions, the radio link repairs losses with FEC.
class UdpSender {
public:
    ~UdpSender();

    /// @param dest <ip>:<port>
    bool open(const std::string &dest);

    /// Send TS packets, 7 per datagram.
    bool sendTs(const uint8_t *data, size_t size);

    /// Send one datagram, e.g. an RTP packet.
    bool sendDatagram(const uint8_t *data, size_t size);

private:
    int m_sock{-1};
};
