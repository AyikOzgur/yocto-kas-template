#include "RtpJpegSender.h"

#include <algorithm>
#include <cstring>
#include <iostream>

namespace {

const size_t JPEG_HEADER_SIZE = 8;
// Quantization table header + luma and chroma 8-bit tables, first packet only.
const size_t QTABLE_HEADER_SIZE = 4 + 2 * 64;

uint16_t be16(const uint8_t *p) {
    return (p[0] << 8) | p[1];
}

} // namespace

bool RtpJpegSender::send(const uint8_t *jpeg, size_t size, uint32_t timestamp) {
    if (size < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8)
        return false;

    uint8_t qtables[4][64];
    bool have_qtable[4] = {false, false, false, false};
    int width = 0, height = 0, type = -1;
    // Quantization table used by luma and by both chroma components.
    int luma_table = -1, chroma_table = -1;
    size_t scan = 0;

    // Walk the marker segments up to the entropy-coded scan.
    for (size_t pos = 2; pos + 4 <= size && scan == 0;) {
        if (jpeg[pos] != 0xFF)
            return false;
        const uint8_t marker = jpeg[pos + 1];
        const size_t body = pos + 4;
        const size_t end = pos + 2 + be16(jpeg + pos + 2);
        if (end > size)
            return false;

        switch (marker) {
        case 0xDB: // DQT, one or more tables
            for (size_t p = body; p + 65 <= end; p += 65) {
                const int precision = jpeg[p] >> 4, id = jpeg[p] & 0x0F;
                if (precision != 0 || id > 3)
                    return false;
                std::memcpy(qtables[id], jpeg + p + 1, 64);
                have_qtable[id] = true;
            }
            break;
        case 0xC0: { // SOF0: precision, height, width, components (id, sampling, table)
            if (end - body < 15 || jpeg[body + 5] != 3)
                return false;
            height = be16(jpeg + body + 1);
            width = be16(jpeg + body + 3);
            const uint8_t y = jpeg[body + 7], cb = jpeg[body + 10], cr = jpeg[body + 13];
            if (cb != 0x11 || cr != 0x11 || jpeg[body + 11] != jpeg[body + 14])
                return false;
            type = y == 0x21 ? 0 : y == 0x22 ? 1 : -1;
            luma_table = jpeg[body + 8] & 0x03;
            chroma_table = jpeg[body + 11] & 0x03;
            break;
        }
        case 0xDD: // DRI, restart markers need RTP type 64+ with an extra header
            std::cerr << "[rtp] restart markers not supported" << std::endl;
            return false;
        case 0xDA: // SOS, the scan follows the segment
            scan = end;
            break;
        default:
            break;
        }
        pos = end;
    }

    if (scan == 0 || type < 0 || !have_qtable[luma_table] || !have_qtable[chroma_table] ||
        width <= 0 || height <= 0 || width > 2040 || height > 2040) {
        std::cerr << "[rtp] unsupported JPEG" << std::endl;
        return false;
    }

    // Receivers append EOI themselves.
    size_t scan_end = size;
    if (scan_end >= scan + 2 && jpeg[scan_end - 2] == 0xFF && jpeg[scan_end - 1] == 0xD9)
        scan_end -= 2;
    const size_t total = scan_end - scan;

    for (size_t offset = 0; offset < total;) {
        uint8_t *j = payload();
        const size_t header = JPEG_HEADER_SIZE + (offset == 0 ? QTABLE_HEADER_SIZE : 0);
        const size_t chunk = std::min(total - offset, MAX_PAYLOAD_SIZE - header);

        // JPEG header: type-specific, 24-bit fragment offset, type, Q, size in 8 pixel blocks.
        j[0] = 0;
        j[1] = offset >> 16;
        j[2] = offset >> 8;
        j[3] = offset;
        j[4] = type;
        j[5] = 255;
        j[6] = width / 8;
        j[7] = height / 8;

        size_t at = JPEG_HEADER_SIZE;
        if (offset == 0) {
            j[at] = 0;     // MBZ
            j[at + 1] = 0; // precision: 8-bit tables
            j[at + 2] = 0;
            j[at + 3] = 2 * 64; // length
            // RFC 2435 types 0/1: first table is luma, second is chroma.
            std::memcpy(j + at + 4, qtables[luma_table], 64);
            std::memcpy(j + at + 4 + 64, qtables[chroma_table], 64);
            at += QTABLE_HEADER_SIZE;
        }
        std::memcpy(j + at, jpeg + scan + offset, chunk);

        offset += chunk;
        if (!sendPacket(at + chunk, offset == total, timestamp))
            return false;
    }
    return true;
}
