#pragma once
#include <cstdint>
#include <vector>

/// Hardware encoder fed with YUYV dma-bufs (stride width * 2), no copies.
class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;

    /// Open for width x height YUYV input.
    virtual bool open(int width, int height) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;

    /** @brief Encode one frame, blocks until the hardware is done.
     *  @param dma_buf_fd YUYV frame, the caller keeps ownership.
     *  @param length Size of the frame in bytes.
     *  @return Encoded frame (JPEG image or H.264/H.265 Annex-B access unit), valid
     *          until the next call. Empty on error, the encoder is closed then and
     *          must be opened again.
     */
    virtual const std::vector<uint8_t> &encode(int dma_buf_fd, uint32_t length) = 0;
};
