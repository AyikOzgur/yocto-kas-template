#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "SocketClient.h"

SocketClient::~SocketClient() {
    close();
}

bool SocketClient::init(const std::string &socketPath) {
    if (socketPath.empty()) {
        std::cerr << "[sock] socket path cannot be empty\n";
        return false;
    }

    m_socketPath = socketPath;
    m_running.store(true);
    m_recvThread = std::thread(&SocketClient::internalRecvThreadFunc, this);
    return true;
}

bool SocketClient::recvFrame(FrameHeader &hdr, int &dmaFd, int timeoutMs) {
    std::unique_lock<std::mutex> lock(m_frameMutex);

    if (timeoutMs < 0) {
        // Blocking - wait until a frame is available or we're shutting down
        m_frameCv.wait(lock, [this] { return m_frameReady.load() || !m_running.load(); });
    } else if (timeoutMs > 0) {
        // Timed wait
        m_frameCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                           [this] { return m_frameReady.load() || !m_running.load(); });
    }
    // timeoutMs == 0: non-blocking, just check

    if (!m_frameReady.load())
        return false;

    hdr = m_latestHdr;
    dmaFd = m_latestDmaFd;
    m_latestDmaFd = -1;
    m_frameReady.store(false);
    return true;
}

bool SocketClient::isConnected() const {
    return m_isConnected.load();
}

void SocketClient::close() {
    m_running.store(false);

    // Wake up any waiting recvFrame call (must hold mutex to avoid missed-wake
    // race)
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_frameCv.notify_all();
    }

    if (m_recvThread.joinable())
        m_recvThread.join();

    if (m_sockFd >= 0) {
        ::close(m_sockFd);
        m_sockFd = -1;
    }
    m_isConnected.store(false);

    // Clean up any unconsumed frame fd
    if (m_latestDmaFd >= 0) {
        ::close(m_latestDmaFd);
        m_latestDmaFd = -1;
    }
    m_frameReady.store(false);
}

bool SocketClient::connectToServer() {
    if (m_sockFd >= 0) {
        ::close(m_sockFd);
        m_sockFd = -1;
    }

    m_sockFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_sockFd < 0) {
        std::cerr << "[sock] socket() failed: " << strerror(errno) << "\n";
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (m_socketPath.size() >= sizeof(addr.sun_path)) {
        std::cerr << "[sock] socket path too long\n";
        ::close(m_sockFd);
        m_sockFd = -1;
        return false;
    }
    std::strncpy(addr.sun_path, m_socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(m_sockFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        ::close(m_sockFd);
        m_sockFd = -1;
        return false;
    }

    m_isConnected.store(true);
    std::cout << "[sock] connected to " << m_socketPath << "\n";
    return true;
}

void SocketClient::internalRecvThreadFunc() {
    while (m_running.load()) {
        // Connect/reconnect loop
        if (!m_isConnected.load()) {
            if (!connectToServer()) {
                // Wait 3 seconds before retrying, but check m_running periodically
                for (int i = 0; i < 30 && m_running.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }

        // Poll for data with 100ms timeout so we can check m_running
        pollfd pfd{};
        pfd.fd = m_sockFd;
        pfd.events = POLLIN;

        int ret = ::poll(&pfd, 1, 100);
        if (ret <= 0)
            continue;

        // Receive frame
        FrameHeader hdr{};
        iovec iov{};
        iov.iov_base = &hdr;
        iov.iov_len = sizeof(hdr);

        union {
            cmsghdr cm;
            char buf[CMSG_SPACE(sizeof(int))];
        } ctrl{};

        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrl.buf;
        msg.msg_controllen = sizeof(ctrl.buf);

        ssize_t n = ::recvmsg(m_sockFd, &msg, MSG_DONTWAIT);
        if (n <= 0) {
            if (n == 0) {
                std::cerr << "[sock] server closed connection\n";
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "[sock] recvmsg failed: " << strerror(errno) << "\n";
            } else {
                continue; // EAGAIN - just loop around
            }
            m_isConnected.store(false);
            ::close(m_sockFd);
            m_sockFd = -1;
            continue;
        }

        if (static_cast<size_t>(n) < sizeof(hdr)) {
            std::cerr << "[sock] short read: " << n << " bytes\n";
            continue;
        }

        int dmaFd = -1;
        cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
            std::memcpy(&dmaFd, CMSG_DATA(cmsg), sizeof(int));
        } else {
            std::cerr << "[sock] no fd in ancillary data\n";
            continue;
        }

        // Store latest frame, close previous if unconsumed
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            if (m_latestDmaFd >= 0)
                ::close(m_latestDmaFd);
            m_latestHdr = hdr;
            m_latestDmaFd = dmaFd;
            m_frameReady.store(true);
        }
        m_frameCv.notify_one();
    }
}
