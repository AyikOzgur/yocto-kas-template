#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

/// Minimal MPEG-TS muxer for one H.264 / H.265 video stream, the container SRT
/// receivers (ffplay, VLC, GStreamer, OBS) expect. One program, no audio.
class TsMuxer {
public:
    explicit TsMuxer(bool hevc) : m_hevc(hevc) {}

    /** @brief Mux one Annex-B access unit into 188 byte TS packets.
     *  PAT/PMT go before every keyframe, so receivers can start there.
     *  @param pts Presentation time, 90 kHz clock. Also used as PCR.
     *  @return TS packets, valid until the next call.
     */
    const std::vector<uint8_t> &mux(const uint8_t *data, size_t size, uint64_t pts);

private:
    bool m_hevc;
    uint8_t m_continuity[3]{}; // PAT, PMT, video
    std::vector<uint8_t> m_pes;
    std::vector<uint8_t> m_out;

    bool isKeyframe(const uint8_t *data, size_t size) const;
    void writeSection(int stream, const std::vector<uint8_t> &section);
    /// Append one TS packet, returns how many payload bytes went in. pcr < 0: none.
    size_t writePacket(int stream, bool unit_start, const uint8_t *payload, size_t size, int64_t pcr = -1,
                       bool random_access = false);
};
