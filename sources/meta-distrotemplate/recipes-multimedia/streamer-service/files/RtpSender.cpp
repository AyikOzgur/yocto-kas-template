#include "RtpSender.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <random>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

RtpSender::~RtpSender() {
    if (m_sock >= 0)
        ::close(m_sock);
}

bool RtpSender::open(const std::string &ip, uint16_t port) {
    m_dest.sin_family = AF_INET;
    m_dest.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &m_dest.sin_addr) != 1) {
        std::cerr << "[rtp] invalid address " << ip << std::endl;
        return false;
    }

    m_sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (m_sock < 0) {
        std::cerr << "[rtp] socket() failed: " << strerror(errno) << std::endl;
        return false;
    }
    // Room for a whole frame burst (a keyframe can be ~100 packets).
    int sndbuf = 1 << 20;
    setsockopt(m_sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    std::random_device rd;
    m_ssrc = rd();
    m_sequence = rd();
    m_packet.resize(MAX_PACKET_SIZE);
    return true;
}

bool RtpSender::sendPacket(size_t payload_size, bool marker, uint32_t timestamp) {
    if (m_sock < 0)
        return false;

    // Version 2, no padding/extension/CSRC.
    uint8_t *p = m_packet.data();
    p[0] = 0x80;
    p[1] = (marker ? 0x80 : 0x00) | m_payloadType;
    p[2] = m_sequence >> 8;
    p[3] = m_sequence;
    p[4] = timestamp >> 24;
    p[5] = timestamp >> 16;
    p[6] = timestamp >> 8;
    p[7] = timestamp;
    p[8] = m_ssrc >> 24;
    p[9] = m_ssrc >> 16;
    p[10] = m_ssrc >> 8;
    p[11] = m_ssrc;
    m_sequence++;

    if (::sendto(m_sock, p, RTP_HEADER_SIZE + payload_size, 0, reinterpret_cast<sockaddr *>(&m_dest),
                 sizeof(m_dest)) < 0) {
        // Report once per error kind, e.g. while the network is down.
        if (errno != m_lastErrno)
            std::cerr << "[rtp] sendto failed: " << strerror(errno) << std::endl;
        m_lastErrno = errno;
        return false;
    }
    m_lastErrno = 0;
    return true;
}
