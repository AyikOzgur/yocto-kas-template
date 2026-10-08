#pragma once
#include <cstdint>
#include <sys/types.h>
#include <unordered_map>
#include <vector>

#include "VideoEncoder.h"

/// H.264 / H.265 encoder on the Rockchip VEPU through MPP (vendor kernel, /dev/mpp_service).
/// YUYV dma-bufs are imported into MPP once and reused (no copy, no conversion).
/// Low latency setup: no B frames, one packet per frame, SPS/PPS(/VPS) before every IDR
/// so a receiver can join at any keyframe.
class MppEncoder : public VideoEncoder {
public:
    /** @param hevc H.265 instead of H.264.
     *  @param bitrate_kbps Constant bitrate target.
     *  @param gop Frames between IDR frames (join / recovery time).
     *  @param fps Input frame rate, for rate control.
     */
    MppEncoder(bool hevc, int bitrate_kbps, int gop, int fps)
        : m_hevc(hevc), m_bitrate_kbps(bitrate_kbps), m_gop(gop), m_fps(fps) {}
    ~MppEncoder() override;

    bool open(int width, int height) override;
    void close() override;
    bool isOpen() const override { return m_ctx != nullptr; }
    int width() const override { return m_width; }
    int height() const override { return m_height; }
    const std::vector<uint8_t> &encode(int dma_buf_fd, uint32_t length) override;

private:
    struct Imported {
        int fd;         // our dup, MPP uses the fd as is and does not take ownership
        void *buffer;   // MppBuffer
    };

    bool m_hevc;
    int m_bitrate_kbps;
    int m_gop;
    int m_fps;
    void *m_ctx{nullptr}; // MppCtx
    void *m_mpi{nullptr}; // MppApi *
    int m_width{0};
    int m_height{0};
    // Imported input buffers keyed by dma-buf inode, a new fd arrives per frame.
    std::unordered_map<ino_t, Imported> m_buffers;
    std::vector<uint8_t> m_stream;

    void releaseBuffers();
};
