#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "FrameHeader.h"


/// @brief SocketClient manages a Unix domain socket connection to receive
/// frames from a producer service.
class SocketClient {
public:
    /** @brief Destructor ensures the socket is properly closed when the
     * SocketClient goes out of scope.
     */
    ~SocketClient();

    /** @brief Connects to the producer service at the specified Unix domain
     * socket path.
     * @param socketPath The filesystem path to the Unix domain socket.
     * @return True if the connection was successful, false otherwise.
     */
    bool init(const std::string &socketPath);

    /** @brief Receives a frame from the producer service. Return always latest
     * frame if multiple frames are received before this function is called again.
     * @param hdr Reference to a FrameHeader struct that will be filled with the
     * received frame's metadata.
     * @param dmaFd Reference to an integer that will be set to the file
     * descriptor of the received frame's DMA buffer.
     * @param timeoutMs Timeout in milliseconds. Values : -1 for blocking until a
     * frame is received, 0 for non-blocking, >0 for blocking with timeout.
     * @return True if a frame was successfully received, false if an error
     * occurred or the connection was lost.
     */
    bool recvFrame(FrameHeader &hdr, int &dmaFd, int timeoutMs = 0);

    /** @brief Checks if the SocketClient is currently connected to the producer
     * service.
     * @return True if connected, false otherwise.
     */
    bool isConnected() const;

    /** @brief Closes the socket connection to the producer service.
     */
    void close();

private:
    /// @brief File descriptor for the Unix domain socket connection. Initialized
    /// to -1 to indicate no connection.
    int m_sockFd{-1};
    /// @brief Atomic boolean flag indicating whether the SocketClient is
    /// currently connected to the producer service.
    std::atomic<bool> m_isConnected{false};
    /// @brief Atomic flag to signal the internal thread to stop.
    std::atomic<bool> m_running{false};
    /// @brief The filesystem path to the Unix domain socket that the SocketClient
    /// is connected to.
    std::string m_socketPath;
    /// @brief Internal receive thread.
    std::thread m_recvThread;

    /// @brief Mutex protecting the latest frame state.
    std::mutex m_frameMutex;
    /// @brief Condition variable signaled when a new frame is available.
    std::condition_variable m_frameCv;
    /// @brief Latest received frame header.
    FrameHeader m_latestHdr{};
    /// @brief Latest received frame DMA file descriptor.
    int m_latestDmaFd{-1};
    /// @brief Whether a new frame is available for consumption.
    std::atomic<bool> m_frameReady{false};

    /** @brief Internal helper function to establish a connection to the producer
     * service.
     * @return True if the connection was successfully established, false
     * otherwise.
     */
    bool connectToServer();

    /** @brief Internal thread function that handles connection management,
     * reconnection, and frame receiving.
     */
    void internalRecvThreadFunc();
};
