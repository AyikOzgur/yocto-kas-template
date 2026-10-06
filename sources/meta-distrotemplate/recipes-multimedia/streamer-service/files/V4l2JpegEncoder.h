#pragma once
#include <cstdint>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <vector>

/// JPEG encoder on a V4L2 mem2mem device (Hantro VEPU on RK3566).
/// Input YUYV frames are imported as dma-bufs (no copy), output is a complete JPEG image.
class V4l2JpegEncoder {
public:
    ~V4l2JpegEncoder();

    /** @brief Open the encoder for width x height YUYV input (stride width * 2).
     *  @param device Video device, empty to use the first JPEG mem2mem encoder.
     *  @param quality JPEG quality, 5..100.
     */
    bool open(const std::string &device, int width, int height, int quality);

    void close();

    bool isOpen() const { return m_fd >= 0; }
    int width() const { return m_width; }
    int height() const { return m_height; }

    /** @brief Encode one frame, blocks until the hardware is done.
     *  @param dma_buf_fd YUYV frame, the caller keeps ownership.
     *  @param length Size of the frame in bytes.
     *  @return JPEG image, valid until the next call. Empty on error, the
     *          encoder is closed then and must be opened again.
     */
    const std::vector<uint8_t> &encode(int dma_buf_fd, uint32_t length);

private:
    struct CaptureBuffer {
        void *data;
        size_t size;
    };

    int m_fd{-1};
    int m_width{0};
    int m_height{0};
    uint32_t m_raw_size{0};
    std::vector<CaptureBuffer> m_capture;
    // OUTPUT buffer index per input dma-buf inode, so vb2 can reuse its import.
    std::unordered_map<ino_t, uint32_t> m_slots;
    std::vector<uint8_t> m_jpeg;
};
