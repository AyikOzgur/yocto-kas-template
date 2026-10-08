#pragma once
#include "RtpSender.h"

/// Sends JPEG images as RTP/JPEG (RFC 2435, payload type 26).
///
/// Supports what the Hantro encoder produces: baseline JPEG, 3 components with
/// 4:2:2 (type 0) or 4:2:0 (type 1) sampling, 8-bit quantization tables and no
/// restart markers. Tables are sent in-band in every frame (Q = 255).
class RtpJpegSender : public RtpSender {
public:
    RtpJpegSender() : RtpSender(26) {}

    bool send(const uint8_t *jpeg, size_t size, uint32_t timestamp) override;
};
