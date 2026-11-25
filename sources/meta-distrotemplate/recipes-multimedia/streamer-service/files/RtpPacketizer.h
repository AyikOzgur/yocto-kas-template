#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

/// H.264 (RFC 6184) / H.265 (RFC 7798) RTP packetizer for the wfb-ng link, the format
/// OpenIPC ground stations (PixelPilot on Android) expect: payload type 96 for H.264,
/// 97 for H.265. NAL units that fit go as single NAL unit packets, larger ones as
/// fragmentation units (FU-A / FU). The marker bit ends each access unit.
class RtpPacketizer {
public:
    /// RTP packet including its 12 byte header. 1400 fits one wfb-ng packet.
    static constexpr size_t DEFAULT_MAX_PACKET = 1400;

    using Sink = std::function<void(const uint8_t *packet, size_t size)>;

    explicit RtpPacketizer(bool hevc, size_t max_packet = DEFAULT_MAX_PACKET);

    /** @brief Packetize one Annex-B access unit, sink is called once per RTP packet.
     *  @param timestamp 90 kHz clock, the same for every packet of the access unit.
     */
    void packetize(const uint8_t *data, size_t size, uint32_t timestamp, const Sink &sink);

private:
    bool m_hevc;
    size_t m_max_packet;
    uint16_t m_seq;
    uint32_t m_ssrc;
    std::vector<uint8_t> m_packet;

    void sendNal(const uint8_t *nal, size_t size, uint32_t timestamp, bool last_of_au, const Sink &sink);
    /// Starts m_packet with an RTP header.
    void writeHeader(uint32_t timestamp, bool marker);
};
