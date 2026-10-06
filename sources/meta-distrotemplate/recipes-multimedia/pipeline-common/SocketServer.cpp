#include "SocketServer.h"

#include <filesystem>
#include <system_error>

SocketServer::~SocketServer() {
    close();
}

bool SocketServer::open(const std::string &socket_path) {
    // The socket lives under a runtime dir (e.g. /run/streams) that may not
    // exist yet; bind() would fail with ENOENT otherwise.
    const std::filesystem::path parent = std::filesystem::path(socket_path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            std::cerr << "mkdir(" << parent << ") failed for " << socket_path << ": " << ec.message() << "\n";
            return false;
        }
    }

    ::unlink(socket_path.c_str());
    socketPath = socket_path;

    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        std::cerr << "socket() failed for " << socket_path << "\n";
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind() failed for " << socket_path << "\n";
        ::close(fd);
        return false;
    }

    if (::listen(fd, 5) < 0) {
        std::cerr << "listen() failed for " << socket_path << "\n";
        ::close(fd);
        return false;
    }

    listenFd = fd;
    std::cout << "Listening " << socket_path << "\n";

    m_stop.store(false);
    m_acceptThread = std::thread(&SocketServer::acceptThreadFunc, this);
    return true;
}

void SocketServer::close() {
    m_stop.store(true);
    if (m_acceptThread.joinable())
        m_acceptThread.join();

    std::lock_guard<std::mutex> lk(mtx);
    for (int cfd : clientFds)
        ::close(cfd);
    clientFds.clear();
    if (listenFd >= 0) {
        ::close(listenFd);
        listenFd = -1;
    }
    ::unlink(socketPath.c_str());
}

void SocketServer::sendFrame(const FrameHeader &hdr, int bufFd) {

    std::lock_guard<std::mutex> lk(mtx);

    msghdr msg{};
    iovec iov{};
    iov.iov_base = const_cast<FrameHeader *>(&hdr);
    iov.iov_len = sizeof(hdr);

    union {
        cmsghdr cm;
        char buf[CMSG_SPACE(sizeof(int))];
    } ctrl{};

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl.buf;
    msg.msg_controllen = sizeof(ctrl.buf);

    cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &bufFd, sizeof(int));

    size_t write = 0;
    for (size_t i = 0; i < clientFds.size(); ++i) {
        ssize_t ret = ::sendmsg(clientFds[i], &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (ret >= 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
            if (write != i)
                clientFds[write] = clientFds[i];
            ++write;
        } else {
            ::close(clientFds[i]);
        }
    }
    clientFds.resize(write);
}

void SocketServer::acceptThreadFunc() {
    while (!m_stop.load()) {
        for (;;) {
            int cfd = ::accept4(listenFd, nullptr, nullptr, SOCK_NONBLOCK);
            if (cfd < 0)
                break;
            std::lock_guard<std::mutex> lk(mtx);
            clientFds.push_back(cfd);
            std::cout << "New client (total=" << clientFds.size() << ")\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
