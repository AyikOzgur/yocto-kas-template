#pragma once
#include <cstdint>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <vector>

#include "VideoEncoder.h"

/// JPEG encoder on a V4L2 mem2mem device (Hantro VEPU, mainline kernel).
/// Input YUYV frames are imported as dma-bufs (no copy), output is a complete JPEG image.
class V4l2JpegEncoder : public VideoEncoder {
public:
    /** @param device Video device, empty to use the first JPEG mem2mem encoder.
     *  @param quality JPEG quality, 5..100.
     */
    V4l2JpegEncoder(const std::string &device, int quality) : m_device(device), m_quality(quality) {}
    ~V4l2JpegEncoder() override;

    bool open(int width, int height) override;
    void close() override;
    bool isOpen() const override { return m_fd >= 0; }
    int width() const override { return m_width; }
    int height() const override { return m_height; }
    const std::vector<uint8_t> &encode(int dma_buf_fd, uint32_t length) override;

private:
    struct CaptureBuffer {
        void *data;
        size_t size;
    };

    std::string m_device;
    int m_quality;
    int m_fd{-1};
    int m_width{0};
    int m_height{0};
    uint32_t m_raw_size{0};
    std::vector<CaptureBuffer> m_capture;
    // OUTPUT buffer index per input dma-buf inode, so vb2 can reuse its import.
    std::unordered_map<ino_t, uint32_t> m_slots;
    std::vector<uint8_t> m_jpeg;
};
