#include "RtpH26xSender.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

struct Nal {
    const uint8_t *data;
    size_t size;
};

/// Split an Annex-B stream at 00 00 01 / 00 00 00 01 start codes.
std::vector<Nal> splitNals(const uint8_t *data, size_t size) {
    std::vector<Nal> nals;
    size_t start = 0; // first byte of the current NAL, 0 = none yet
    for (size_t i = 0; i + 3 <= size;) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            if (start) {
                size_t end = i;
                // A 4 byte start code and trailing_zero_8bits belong to the separator.
                while (end > start && data[end - 1] == 0)
                    end--;
                nals.push_back({data + start, end - start});
            }
            i += 3;
            start = i;
        } else {
            i++;
        }
    }
    if (start && start < size)
        nals.push_back({data + start, size - start});
    return nals;
}

} // namespace

bool RtpH26xSender::send(const uint8_t *data, size_t size, uint32_t timestamp) {
    const std::vector<Nal> nals = splitNals(data, size);
    for (size_t i = 0; i < nals.size(); i++) {
        if (!sendNal(nals[i].data, nals[i].size, i + 1 == nals.size(), timestamp))
            return false;
    }
    return !nals.empty();
}

bool RtpH26xSender::sendNal(const uint8_t *nal, size_t size, bool last, uint32_t timestamp) {
    const size_t header_size = m_hevc ? 2 : 1;
    if (size <= header_size)
        return true;

    // Single NAL unit packet.
    if (size <= MAX_PAYLOAD_SIZE) {
        std::memcpy(payload(), nal, size);
        return sendPacket(size, last, timestamp);
    }

    // Fragmentation unit: payload header + FU header, then the NAL body in chunks.
    uint8_t fu_indicator[2];
    uint8_t type;
    if (m_hevc) {
        type = (nal[0] >> 1) & 0x3F;
        fu_indicator[0] = (nal[0] & 0x81) | (49 << 1); // keep F bit and LayerId MSB, type 49 = FU
        fu_indicator[1] = nal[1];                      // LayerId LSBs, TID
    } else {
        type = nal[0] & 0x1F;
        fu_indicator[0] = (nal[0] & 0xE0) | 28; // keep F and NRI, type 28 = FU-A
    }
    const size_t fu_header_size = header_size + 1;
    const size_t max_chunk = MAX_PAYLOAD_SIZE - fu_header_size;

    const uint8_t *body = nal + header_size;
    size_t remaining = size - header_size;
    bool first = true;
    while (remaining > 0) {
        const size_t chunk = std::min(remaining, max_chunk);
        const bool end = chunk == remaining;

        uint8_t *p = payload();
        std::memcpy(p, fu_indicator, header_size);
        p[header_size] = (first ? 0x80 : 0x00) | (end ? 0x40 : 0x00) | type;
        std::memcpy(p + fu_header_size, body, chunk);
        if (!sendPacket(fu_header_size + chunk, last && end, timestamp))
            return false;

        body += chunk;
        remaining -= chunk;
        first = false;
    }
    return true;
}
