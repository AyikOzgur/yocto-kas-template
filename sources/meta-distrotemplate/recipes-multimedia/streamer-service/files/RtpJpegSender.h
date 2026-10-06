#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <netinet/in.h>

/// Sends JPEG images as RTP/JPEG (RFC 2435, payload type 26) over UDP.
///
/// Supports what the Hantro encoder produces: baseline JPEG, 3 components with
/// 4:2:2 (type 0) or 4:2:0 (type 1) sampling, 8-bit quantization tables and no
/// restart markers. Tables are sent in-band in every frame (Q = 255).
class RtpJpegSender {
public:
    ~RtpJpegSender();

    bool open(const std::string &ip, uint16_t port);

    /** @brief Packetize and send one JPEG image.
     *  @param timestamp RTP timestamp, 90 kHz clock.
     */
    bool send(const uint8_t *jpeg, size_t size, uint32_t timestamp);

private:
    int m_sock{-1};
    sockaddr_in m_dest{};
    uint16_t m_sequence{0};
    uint32_t m_ssrc{0};
    int m_lastErrno{0};
    std::vector<uint8_t> m_packet;
};
