#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include <srt/srt.h>

/// SRT listener in live mode. Any number of receivers (SRT callers) can connect from
/// anywhere and get the same stream; lost packets are retransmitted within the latency.
class SrtServer {
public:
    ~SrtServer();

    /** @param port UDP port to listen on, all interfaces.
     *  @param latency_ms Receive buffer for retransmissions, the larger of both sides is used.
     */
    bool listen(int port, int latency_ms);

    /// Accept waiting receivers without blocking, returns how many joined.
    int acceptClients();

    size_t clients() const { return m_clients.size(); }

    /// Send MPEG-TS to every receiver, drops the ones that disconnected.
    /// @return false if data had to be dropped for a receiver (send buffer full).
    bool send(const uint8_t *data, size_t size);

private:
    SRTSOCKET m_listener{SRT_INVALID_SOCK};
    std::vector<SRTSOCKET> m_clients;
};
