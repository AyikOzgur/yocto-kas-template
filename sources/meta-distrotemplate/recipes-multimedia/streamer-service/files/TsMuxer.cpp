#include "TsMuxer.h"

#include <algorithm>
#include <cstring>

namespace {

const size_t TS_PACKET_SIZE = 188;
const size_t TS_PAYLOAD_SIZE = TS_PACKET_SIZE - 4;

enum Stream { PAT, PMT, VIDEO };
const uint16_t PIDS[] = {0x0000, 0x1000, 0x0100};

const uint8_t STREAM_TYPE_H264 = 0x1b;
const uint8_t STREAM_TYPE_H265 = 0x24;

/// CRC-32/MPEG-2 of PSI sections.
uint32_t crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xffffffff;
    for (size_t i = 0; i < size; i++) {
        crc ^= uint32_t(data[i]) << 24;
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04c11db7 : crc << 1;
    }
    return crc;
}

} // namespace

bool TsMuxer::isKeyframe(const uint8_t *data, size_t size) const {
    // MPP puts the parameter sets in front of every IDR, so a keyframe starts with
    // an SPS (H.264) or a VPS (H.265).
    size_t i = 0;
    while (i < size && data[i] == 0)
        i++;
    if (i < 2 || i + 1 >= size || data[i] != 1)
        return false;
    const uint8_t nal = data[i + 1];
    return m_hevc ? ((nal >> 1) & 0x3f) == 32 : (nal & 0x1f) == 7;
}

size_t TsMuxer::writePacket(int stream, bool unit_start, const uint8_t *payload, size_t size, int64_t pcr,
                            bool random_access) {
    const uint16_t pid = PIDS[stream];
    const size_t offset = m_out.size();
    m_out.resize(offset + TS_PACKET_SIZE, 0xff);
    uint8_t *p = m_out.data() + offset;

    // Adaptation field for PCR / random access flags, and as stuffing when the payload is short.
    const size_t min_adaptation = pcr >= 0 ? 8 : random_access ? 2 : 0;
    const size_t n = std::min(size, TS_PAYLOAD_SIZE - min_adaptation);
    const size_t adaptation = TS_PAYLOAD_SIZE - n;

    p[0] = 0x47;
    p[1] = (unit_start ? 0x40 : 0) | (pid >> 8);
    p[2] = pid & 0xff;
    p[3] = (adaptation ? 0x30 : 0x10) | m_continuity[stream];
    m_continuity[stream] = (m_continuity[stream] + 1) & 0x0f;

    if (adaptation) {
        p[4] = adaptation - 1;
        if (adaptation > 1)
            p[5] = (random_access ? 0x40 : 0) | (pcr >= 0 ? 0x10 : 0);
        if (pcr >= 0) {
            // 33 bit base in 90 kHz, extension 0.
            const uint64_t base = pcr & 0x1ffffffffULL;
            p[6] = base >> 25;
            p[7] = base >> 17;
            p[8] = base >> 9;
            p[9] = base >> 1;
            p[10] = ((base & 1) << 7) | 0x7e;
            p[11] = 0;
        }
        // The rest of the adaptation field stays 0xff stuffing.
    }
    std::memcpy(p + 4 + adaptation, payload, n);
    return n;
}

void TsMuxer::writeSection(int stream, const std::vector<uint8_t> &section) {
    // pointer_field, section, CRC.
    std::vector<uint8_t> payload{0x00};
    payload.insert(payload.end(), section.begin(), section.end());
    const uint32_t crc = crc32(section.data(), section.size());
    payload.insert(payload.end(), {uint8_t(crc >> 24), uint8_t(crc >> 16), uint8_t(crc >> 8), uint8_t(crc)});
    writePacket(stream, true, payload.data(), payload.size());
}

const std::vector<uint8_t> &TsMuxer::mux(const uint8_t *data, size_t size, uint64_t pts) {
    m_out.clear();
    const bool keyframe = isKeyframe(data, size);

    if (keyframe) {
        const uint16_t pmt = PIDS[PMT], video = PIDS[VIDEO];
        // Program 1 -> PMT.
        writeSection(PAT, {0x00, 0xb0, 13, 0x00, 0x01, 0xc1, 0x00, 0x00, 0x00, 0x01, uint8_t(0xe0 | (pmt >> 8)),
                           uint8_t(pmt & 0xff)});
        // One video stream, also carrying the PCR.
        writeSection(PMT, {0x02, 0xb0, 18, 0x00, 0x01, 0xc1, 0x00, 0x00, uint8_t(0xe0 | (video >> 8)),
                           uint8_t(video & 0xff), 0xf0, 0x00, m_hevc ? STREAM_TYPE_H265 : STREAM_TYPE_H264,
                           uint8_t(0xe0 | (video >> 8)), uint8_t(video & 0xff), 0xf0, 0x00});
    }

    // PES header: video stream 0xe0, unbounded length, PTS only.
    pts &= 0x1ffffffffULL;
    m_pes = {0x00, 0x00, 0x01, 0xe0, 0x00, 0x00, 0x80, 0x80, 0x05,
             uint8_t(0x21 | ((pts >> 29) & 0x0e)), uint8_t(pts >> 22), uint8_t(0x01 | ((pts >> 14) & 0xfe)),
             uint8_t(pts >> 7), uint8_t(0x01 | ((pts << 1) & 0xfe))};
    // Access unit delimiter, required in front of every access unit in TS.
    if (m_hevc)
        m_pes.insert(m_pes.end(), {0x00, 0x00, 0x00, 0x01, 0x46, 0x01, 0x50});
    else
        m_pes.insert(m_pes.end(), {0x00, 0x00, 0x00, 0x01, 0x09, 0xf0});
    m_pes.insert(m_pes.end(), data, data + size);

    size_t done = writePacket(VIDEO, true, m_pes.data(), m_pes.size(), pts, keyframe);
    while (done < m_pes.size())
        done += writePacket(VIDEO, false, m_pes.data() + done, m_pes.size() - done);
    return m_out;
}
