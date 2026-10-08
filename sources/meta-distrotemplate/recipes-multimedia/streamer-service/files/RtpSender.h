#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <netinet/in.h>

/// RTP over UDP: socket, RTP header, sequence numbers. Subclasses packetize one
/// encoded frame per send() by writing payloads straight into the packet buffer.
class RtpSender {
public:
    explicit RtpSender(uint8_t payload_type) : m_payloadType(payload_type) {}
    virtual ~RtpSender();

    bool open(const std::string &ip, uint16_t port);

    /** @brief Packetize and send one encoded frame.
     *  @param timestamp RTP timestamp, 90 kHz clock.
     */
    virtual bool send(const uint8_t *data, size_t size, uint32_t timestamp) = 0;

protected:
    static const size_t MAX_PACKET_SIZE = 1400; // fits a 1500 MTU with IP/UDP headers
    static const size_t RTP_HEADER_SIZE = 12;
    static const size_t MAX_PAYLOAD_SIZE = MAX_PACKET_SIZE - RTP_HEADER_SIZE;

    /// Payload area of the next packet, MAX_PAYLOAD_SIZE bytes.
    uint8_t *payload() { return m_packet.data() + RTP_HEADER_SIZE; }

    /// Send the packet with payload_size bytes in payload(). Marker = last packet of a frame.
    bool sendPacket(size_t payload_size, bool marker, uint32_t timestamp);

private:
    uint8_t m_payloadType;
    int m_sock{-1};
    sockaddr_in m_dest{};
    uint16_t m_sequence{0};
    uint32_t m_ssrc{0};
    int m_lastErrno{0};
    std::vector<uint8_t> m_packet;
};
