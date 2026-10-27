#pragma once
#include <algorithm>

#include "SrtServer.h"

/// Adapts the encoder bitrate to the SRT link, fed once per second: cut by 30% as soon
/// as the link struggles, add 10% of the maximum after it has been clean for a while
/// (AIMD, like TCP). Random Wi-Fi loss that SRT recovers in time is not a reason to cut.
class BitrateController {
public:
    /** @param min_kbps Floor, min_kbps >= max_kbps keeps the bitrate fixed.
     *  @param max_kbps Ceiling and starting point.
     *  @param latency_ms SRT latency, the send buffer limit before packets get dropped.
     */
    BitrateController(int min_kbps, int max_kbps, int latency_ms)
        : m_min(std::min(min_kbps, max_kbps)), m_max(max_kbps), m_latency(latency_ms), m_kbps(max_kbps) {}

    /// @return Bitrate to use from now on.
    int update(const LinkStats &link) {
        // SRT drops packets that stay in the send buffer longer than about latency + 20 ms.
        const bool buffer_filling = link.send_buffer_ms > (m_latency + 20) * 3 / 4;
        const bool many_retransmits = link.retransmitted * 100 > link.sent * MAX_RETRANSMIT_PERCENT;
        if (link.dropped > 0 || buffer_filling || many_retransmits) {
            m_kbps = std::max(m_min, m_kbps * 7 / 10);
            m_clean_seconds = 0;
        } else if (++m_clean_seconds >= CLEAN_SECONDS_BEFORE_RAISE) {
            m_kbps = std::min(m_max, m_kbps + m_max / 10);
            m_clean_seconds = 0;
        }
        return m_kbps;
    }

private:
    static const int MAX_RETRANSMIT_PERCENT = 10;
    static const int CLEAN_SECONDS_BEFORE_RAISE = 2;

    int m_min;
    int m_max;
    int m_latency;
    int m_kbps;
    int m_clean_seconds{0};
};
