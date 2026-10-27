#include "SrtServer.h"

#include <algorithm>
#include <iostream>

#include <arpa/inet.h>
#include <netinet/in.h>

namespace {

// 7 TS packets, the SRT live mode payload size.
const size_t CHUNK_SIZE = 7 * 188;

} // namespace

SrtServer::~SrtServer() {
    for (SRTSOCKET client : m_clients)
        srt_close(client);
    if (m_listener != SRT_INVALID_SOCK)
        srt_close(m_listener);
    srt_cleanup();
}

bool SrtServer::listen(int port, int latency_ms) {
    srt_startup();
    m_listener = srt_create_socket();

    // Non-blocking accept, and non-blocking send on the accepted sockets (inherited),
    // so a slow receiver never stalls the encoder.
    const bool no = false;
    srt_setsockflag(m_listener, SRTO_RCVSYN, &no, sizeof(no));
    srt_setsockflag(m_listener, SRTO_SNDSYN, &no, sizeof(no));
    srt_setsockflag(m_listener, SRTO_LATENCY, &latency_ms, sizeof(latency_ms));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (srt_bind(m_listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SRT_ERROR ||
        srt_listen(m_listener, 4) == SRT_ERROR) {
        std::cerr << "[srt] can not listen on port " << port << ": " << srt_getlasterror_str() << std::endl;
        return false;
    }
    return true;
}

int SrtServer::acceptClients() {
    int joined = 0;
    for (;;) {
        sockaddr_in peer{};
        int len = sizeof(peer);
        SRTSOCKET client = srt_accept(m_listener, reinterpret_cast<sockaddr *>(&peer), &len);
        if (client == SRT_INVALID_SOCK)
            break;
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        std::cout << "[srt] client " << ip << ":" << ntohs(peer.sin_port) << " connected" << std::endl;
        m_clients.push_back(client);
        joined++;
    }
    return joined;
}

bool SrtServer::send(const uint8_t *data, size_t size) {
    bool complete = true;
    for (auto it = m_clients.begin(); it != m_clients.end();) {
        bool connected = true;
        for (size_t offset = 0; offset < size && connected; offset += CHUNK_SIZE) {
            const int n = std::min(CHUNK_SIZE, size - offset);
            if (srt_sendmsg2(*it, reinterpret_cast<const char *>(data + offset), n, nullptr) != SRT_ERROR)
                continue;
            if (srt_getlasterror(nullptr) == SRT_EASYNCSND)
                complete = false;
            else
                connected = false;
        }
        if (connected) {
            ++it;
        } else {
            std::cout << "[srt] client disconnected: " << srt_getlasterror_str() << std::endl;
            srt_close(*it);
            it = m_clients.erase(it);
        }
    }
    return complete;
}
