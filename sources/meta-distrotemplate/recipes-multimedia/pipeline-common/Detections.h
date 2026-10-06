#pragma once
// Detection results from inference-service to its consumers (renderer-service).
//
// SOCK_SEQPACKET unix socket, one message per inference:
// DetectionsHeader followed by `count` DetectionBox entries. Boxes are in the
// model input space (letterboxed modelWidth x modelHeight), consumers map them
// back to their frame size with the same letterbox as preprocessing-service.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const char *DETECTIONS_SOCKET_PATH = "/run/inference-service.sock";
static const int MAX_DETECTIONS = 64;

struct __attribute__((packed)) DetectionsHeader {
    /// @brief Frame id of the camera frame the detections belong to.
    uint64_t frameId;
    /// @brief Model input size, boxes are in this coordinate space.
    uint16_t modelWidth;
    uint16_t modelHeight;
    /// @brief Number of DetectionBox entries following the header.
    uint16_t count;
};

struct __attribute__((packed)) DetectionBox {
    float x0, y0, x1, y1;
    float score;
    int32_t classId;
};

static_assert(sizeof(DetectionsHeader) == 14, "DetectionsHeader must be 14 bytes");
static_assert(sizeof(DetectionBox) == 24, "DetectionBox must be 24 bytes");

/// @brief Publishes detection messages to all connected clients, never blocks.
class DetectionPublisher {
public:
    ~DetectionPublisher() { close(); }

    bool open(const std::string &path) {
        ::unlink(path.c_str());
        m_listenFd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
        if (m_listenFd < 0)
            return false;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (::bind(m_listenFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 ||
            ::listen(m_listenFd, 4) < 0) {
            ::close(m_listenFd);
            m_listenFd = -1;
            return false;
        }
        m_path = path;
        return true;
    }

    void publish(const DetectionsHeader &hdr, const DetectionBox *boxes) {
        if (m_listenFd < 0)
            return;
        for (int fd; (fd = ::accept4(m_listenFd, nullptr, nullptr, SOCK_NONBLOCK)) >= 0;)
            m_clients.push_back(fd);
        if (m_clients.empty())
            return;

        m_buffer.resize(sizeof(hdr) + hdr.count * sizeof(DetectionBox));
        std::memcpy(m_buffer.data(), &hdr, sizeof(hdr));
        std::memcpy(m_buffer.data() + sizeof(hdr), boxes, hdr.count * sizeof(DetectionBox));

        size_t keep = 0;
        for (size_t i = 0; i < m_clients.size(); i++) {
            ssize_t ret = ::send(m_clients[i], m_buffer.data(), m_buffer.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
            if (ret >= 0 || errno == EAGAIN || errno == EWOULDBLOCK)
                m_clients[keep++] = m_clients[i];
            else
                ::close(m_clients[i]);
        }
        m_clients.resize(keep);
    }

    void close() {
        for (int fd : m_clients)
            ::close(fd);
        m_clients.clear();
        if (m_listenFd >= 0) {
            ::close(m_listenFd);
            m_listenFd = -1;
            ::unlink(m_path.c_str());
        }
    }

private:
    std::string m_path;
    int m_listenFd{-1};
    std::vector<int> m_clients;
    std::vector<uint8_t> m_buffer;
};

/// @brief Receives detection messages, reconnecting in the background. Never blocks.
class DetectionSubscriber {
public:
    ~DetectionSubscriber() {
        if (m_fd >= 0)
            ::close(m_fd);
    }

    /** @brief Fetch the newest message received since the last call.
     *  @return true if a new message was received.
     */
    bool poll(const std::string &path, DetectionsHeader &hdr, std::vector<DetectionBox> &boxes) {
        if (m_fd < 0 && !connect(path))
            return false;

        bool got = false;
        uint8_t buf[sizeof(DetectionsHeader) + MAX_DETECTIONS * sizeof(DetectionBox)];
        for (;;) {
            ssize_t n = ::recv(m_fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            if (n <= 0) {
                std::cerr << "[detections] disconnected\n";
                ::close(m_fd);
                m_fd = -1;
                break;
            }
            if (static_cast<size_t>(n) < sizeof(DetectionsHeader))
                continue;
            std::memcpy(&hdr, buf, sizeof(hdr));
            size_t count = std::min<size_t>(hdr.count, (n - sizeof(hdr)) / sizeof(DetectionBox));
            boxes.resize(count);
            std::memcpy(boxes.data(), buf + sizeof(hdr), count * sizeof(DetectionBox));
            hdr.count = static_cast<uint16_t>(count);
            got = true;
        }
        return got;
    }

private:
    int m_fd{-1};
    std::chrono::steady_clock::time_point m_lastAttempt{};

    bool connect(const std::string &path) {
        auto now = std::chrono::steady_clock::now();
        if (now - m_lastAttempt < std::chrono::seconds(1))
            return false;
        m_lastAttempt = now;

        m_fd = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (m_fd < 0)
            return false;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(m_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
            ::close(m_fd);
            m_fd = -1;
            return false;
        }
        std::cout << "[detections] connected to " << path << "\n";
        return true;
    }
};
