#pragma once
#include "RtpSender.h"

/// Sends H.264 (RFC 6184) or H.265 (RFC 7798) access units over RTP, payload type 96.
///
/// Input is one Annex-B access unit per send() (start code separated NAL units).
/// NAL units that fit go as single NAL unit packets, larger ones as FU-A (H.264)
/// or FU (H.265) fragments. The marker bit is set on the last packet of the access
/// unit so receivers can decode as soon as a frame is complete.
class RtpH26xSender : public RtpSender {
public:
    explicit RtpH26xSender(bool hevc) : RtpSender(96), m_hevc(hevc) {}

    bool send(const uint8_t *data, size_t size, uint32_t timestamp) override;

private:
    bool m_hevc;

    bool sendNal(const uint8_t *nal, size_t size, bool last, uint32_t timestamp);
};
