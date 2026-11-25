#include "RtpPacketizer.h"

#include <algorithm>
#include <random>

namespace {

const size_t RTP_HEADER_SIZE = 12;
const uint8_t PAYLOAD_TYPE_H264 = 96;
const uint8_t PAYLOAD_TYPE_H265 = 97;
const uint8_t FU_A_TYPE_H264 = 28;
const uint8_t FU_TYPE_H265 = 49;
const uint8_t FU_START = 0x80;
const uint8_t FU_END = 0x40;

/// Position of the next 00 00 01 start code at or after pos, size if there is none.
size_t findStartCode(const uint8_t *data, size_t size, size_t pos) {
    for (; pos + 3 <= size; pos++) {
        if (data[pos + 2] > 1) {
            pos += 2; // no start code can begin before pos + 3
        } else if (data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1) {
            return pos;
        }
    }
    return size;
}

} // namespace

RtpPacketizer::RtpPacketizer(bool hevc, size_t max_packet) : m_hevc(hevc), m_max_packet(max_packet) {
    // Random start values (RFC 3550), a restarted streamer is a new stream to the receiver.
    std::random_device random;
    m_seq = static_cast<uint16_t>(random());
    m_ssrc = random();
    m_packet.reserve(m_max_packet);
}

void RtpPacketizer::packetize(const uint8_t *data, size_t size, uint32_t timestamp, const Sink &sink) {
    size_t start = findStartCode(data, size, 0);
    while (start < size) {
        const size_t nal = start + 3;
        const size_t next = findStartCode(data, size, nal);
        // The zeros before the next start code (4 byte start code, trailing_zero_8bits)
        // are not part of this NAL unit.
        size_t end = next;
        while (end > nal && data[end - 1] == 0)
            end--;
        if (end > nal)
            sendNal(data + nal, end - nal, timestamp, next >= size, sink);
        start = next;
    }
}

void RtpPacketizer::sendNal(const uint8_t *nal, size_t size, uint32_t timestamp, bool last_of_au,
                            const Sink &sink) {
    const size_t nal_header_size = m_hevc ? 2 : 1;
    if (size <= nal_header_size)
        return;

    // Single NAL unit packet.
    if (RTP_HEADER_SIZE + size <= m_max_packet) {
        writeHeader(timestamp, last_of_au);
        m_packet.insert(m_packet.end(), nal, nal + size);
        sink(m_packet.data(), m_packet.size());
        return;
    }

    // Fragmentation units: the NAL header is replaced by the FU indicator / payload header
    // (same F and NRI / layer and TID bits, FU type) and an FU header with the original type.
    uint8_t fu_prefix[3];
    size_t fu_prefix_size;
    uint8_t nal_type;
    if (m_hevc) {
        fu_prefix[0] = (nal[0] & 0x81) | (FU_TYPE_H265 << 1);
        fu_prefix[1] = nal[1];
        nal_type = (nal[0] >> 1) & 0x3f;
        fu_prefix_size = 3;
    } else {
        fu_prefix[0] = (nal[0] & 0xe0) | FU_A_TYPE_H264;
        nal_type = nal[0] & 0x1f;
        fu_prefix_size = 2;
    }
    const size_t chunk_max = m_max_packet - RTP_HEADER_SIZE - fu_prefix_size;
    const uint8_t *payload = nal + nal_header_size;
    size_t left = size - nal_header_size;
    bool first = true;
    while (left > 0) {
        const size_t chunk = std::min(left, chunk_max);
        const bool last = chunk == left;
        fu_prefix[fu_prefix_size - 1] = nal_type | (first ? FU_START : 0) | (last ? FU_END : 0);
        writeHeader(timestamp, last && last_of_au);
        m_packet.insert(m_packet.end(), fu_prefix, fu_prefix + fu_prefix_size);
        m_packet.insert(m_packet.end(), payload, payload + chunk);
        sink(m_packet.data(), m_packet.size());
        payload += chunk;
        left -= chunk;
        first = false;
    }
}

void RtpPacketizer::writeHeader(uint32_t timestamp, bool marker) {
    const uint8_t payload_type = m_hevc ? PAYLOAD_TYPE_H265 : PAYLOAD_TYPE_H264;
    const uint16_t seq = m_seq++;
    m_packet.assign({
        0x80, // version 2, no padding, no extension, no CSRC
        static_cast<uint8_t>((marker ? 0x80 : 0) | payload_type),
        static_cast<uint8_t>(seq >> 8),
        static_cast<uint8_t>(seq),
        static_cast<uint8_t>(timestamp >> 24),
        static_cast<uint8_t>(timestamp >> 16),
        static_cast<uint8_t>(timestamp >> 8),
        static_cast<uint8_t>(timestamp),
        static_cast<uint8_t>(m_ssrc >> 24),
        static_cast<uint8_t>(m_ssrc >> 16),
        static_cast<uint8_t>(m_ssrc >> 8),
        static_cast<uint8_t>(m_ssrc),
    });
}
