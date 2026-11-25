#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include "FrameHeader.h"


/** @brief Socket server for streaming frames. */
class SocketServer {
public:
    ~SocketServer();

    /** @brief Open all endpoints and start accepting connections.
     *  @param socket_path Path to the Unix domain socket.
     *  @return true on success.
     */
    bool open(const std::string &socket_path);

    /** @brief Close all endpoints and stop accepting connections. */
    void close();

    /** @brief Send a frame to all clients connected to the specified stream.
     *  @param stream_name Stream identifier.
     *  @param hdr Frame header containing metadata.
     *  @param bufFd File descriptor of the frame buffer (e.g. DMA-BUF fd).
     */
    void sendFrame(const FrameHeader &hdr, int bufFd);

private:

    /// @brief Path to the Unix domain socket.
    std::string socketPath;
    /// @brief Listening socket file descriptor.
    int listenFd = -1;
    /// @brief Connected client file descriptors.
    std::vector<int> clientFds;
    /// @brief Mutex for synchronizing access to clientFds.
    std::mutex mtx;

    /// @brief Thread for accepting client connections.
    std::thread m_acceptThread;
    /// @brief Atomic flag to signal the accept thread to stop.
    std::atomic<bool> m_stop{false};

    /** @brief Thread function for accepting new client connections on all endpoints. */
    void acceptThreadFunc();
};
