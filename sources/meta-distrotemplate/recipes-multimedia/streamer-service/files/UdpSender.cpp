#include "UdpSender.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// 7 TS packets, the usual TS over UDP datagram size.
const size_t CHUNK_SIZE = 7 * 188;

} // namespace

UdpSender::~UdpSender() {
    if (m_sock >= 0)
        ::close(m_sock);
}

bool UdpSender::open(const std::string &dest) {
    const size_t colon = dest.rfind(':');
    const int port = colon == std::string::npos ? 0 : std::atoi(dest.c_str() + colon + 1);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (port <= 0 || port > 65535 || inet_pton(AF_INET, dest.substr(0, colon).c_str(), &addr.sin_addr) != 1) {
        std::cerr << "[udp] invalid destination " << dest << ", expected <ip>:<port>" << std::endl;
        return false;
    }
    m_sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (m_sock < 0 || ::connect(m_sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[udp] can not open socket to " << dest << ": " << strerror(errno) << std::endl;
        return false;
    }
    return true;
}

bool UdpSender::send(const uint8_t *data, size_t size) {
    bool ok = true;
    for (size_t offset = 0; offset < size; offset += CHUNK_SIZE) {
        // Fails (ECONNREFUSED) while nobody listens, e.g. no Wi-Fi card plugged in; keep going.
        if (::send(m_sock, data + offset, std::min(CHUNK_SIZE, size - offset), 0) < 0)
            ok = false;
    }
    return ok;
}
