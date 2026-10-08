#define MODULE_TAG "streamer"

#include "MppEncoder.h"

#include <iostream>

#include <sys/stat.h>
#include <unistd.h>

#include <rockchip/mpp_buffer.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_venc_cfg.h>

namespace {

// Input buffers of the overlay ring, more means the source keeps allocating new ones.
const size_t MAX_IMPORTED = 8;

MppCtx ctx(void *p) {
    return static_cast<MppCtx>(p);
}

MppApi *api(void *p) {
    return static_cast<MppApi *>(p);
}

} // namespace

MppEncoder::~MppEncoder() {
    close();
}

bool MppEncoder::open(int width, int height) {
    close();

    // VEPU needs the YUYV stride to be the width aligned to 8 pixels; the overlay
    // buffers are tightly packed, so the width must already be aligned.
    if (width % 8 != 0 || height % 2 != 0) {
        std::cerr << "[mpp] unsupported size " << width << "x" << height << std::endl;
        return false;
    }

    MppCtx c = nullptr;
    MppApi *mpi = nullptr;
    const MppCodingType coding = m_hevc ? MPP_VIDEO_CodingHEVC : MPP_VIDEO_CodingAVC;
    if (mpp_create(&c, &mpi) != MPP_OK || mpp_init(c, MPP_CTX_ENC, coding) != MPP_OK) {
        std::cerr << "[mpp] can not create encoder (is /dev/mpp_service there?)" << std::endl;
        if (c)
            mpp_destroy(c);
        return false;
    }
    m_ctx = c;
    m_mpi = mpi;

    MppEncCfg cfg = nullptr;
    mpp_enc_cfg_init(&cfg);
    mpi->control(c, MPP_ENC_GET_CFG, cfg);

    mpp_enc_cfg_set_s32(cfg, "codec:type", coding);
    mpp_enc_cfg_set_s32(cfg, "prep:width", width);
    mpp_enc_cfg_set_s32(cfg, "prep:height", height);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", width * 2); // bytes for packed YUYV
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", height);
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV422_YUYV);
    // Camera YUYV is limited (TV) range.
    mpp_enc_cfg_set_s32(cfg, "prep:range", MPP_FRAME_RANGE_MPEG);

    // Constant bitrate keeps the stream steady over Wi-Fi, frame drops off.
    const int bps = m_bitrate_kbps * 1000;
    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bps);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max", bps * 17 / 16);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min", bps * 15 / 16);
    mpp_enc_cfg_set_u32(cfg, "rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", m_fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", m_fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:gop", m_gop);

    if (!m_hevc) {
        mpp_enc_cfg_set_s32(cfg, "h264:profile", 100); // high
        mpp_enc_cfg_set_s32(cfg, "h264:level", 40);
        mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
        mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc", 0);
        mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", 1);
    }

    MPP_RET ret = mpi->control(c, MPP_ENC_SET_CFG, cfg);
    mpp_enc_cfg_deinit(cfg);
    if (ret != MPP_OK) {
        std::cerr << "[mpp] MPP_ENC_SET_CFG failed: " << ret << std::endl;
        close();
        return false;
    }

    // Parameter sets in-band before every IDR, receivers can start at any keyframe.
    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    if (mpi->control(c, MPP_ENC_SET_HEADER_MODE, &header_mode) != MPP_OK) {
        std::cerr << "[mpp] MPP_ENC_SET_HEADER_MODE failed" << std::endl;
        close();
        return false;
    }

    m_width = width;
    m_height = height;
    std::cout << "[mpp] " << (m_hevc ? "H.265" : "H.264") << " " << width << "x" << height << " YUYV, "
              << m_bitrate_kbps << " kbps CBR, gop " << m_gop << " @ " << m_fps << " fps" << std::endl;
    return true;
}

void MppEncoder::releaseBuffers() {
    for (auto &[key, imported] : m_buffers) {
        MppBuffer buffer = static_cast<MppBuffer>(imported.buffer);
        mpp_buffer_put(buffer);
        ::close(imported.fd);
    }
    m_buffers.clear();
}

void MppEncoder::close() {
    if (!m_ctx)
        return;
    api(m_mpi)->reset(ctx(m_ctx));
    releaseBuffers();
    mpp_destroy(ctx(m_ctx));
    m_ctx = nullptr;
    m_mpi = nullptr;
    m_width = m_height = 0;
}

const std::vector<uint8_t> &MppEncoder::encode(int dma_buf_fd, uint32_t length) {
    m_stream.clear();
    if (!m_ctx)
        return m_stream;

    const uint32_t frame_size = m_width * 2 * m_height;
    struct stat st;
    if (fstat(dma_buf_fd, &st) < 0 || length < frame_size) {
        std::cerr << "[mpp] invalid input frame, length " << length << " < " << frame_size << std::endl;
        return m_stream;
    }

    auto it = m_buffers.find(st.st_ino);
    if (it == m_buffers.end()) {
        if (m_buffers.size() >= MAX_IMPORTED)
            releaseBuffers();

        Imported imported{::dup(dma_buf_fd), nullptr};
        MppBufferInfo info{};
        info.type = MPP_BUFFER_TYPE_EXT_DMA;
        info.fd = imported.fd;
        info.size = length;
        info.index = m_buffers.size();
        MppBuffer buffer = nullptr;
        if (imported.fd < 0 || mpp_buffer_import(&buffer, &info) != MPP_OK) {
            std::cerr << "[mpp] can not import dma-buf" << std::endl;
            if (imported.fd >= 0)
                ::close(imported.fd);
            return m_stream;
        }
        imported.buffer = buffer;
        it = m_buffers.emplace(st.st_ino, imported).first;
    }

    MppFrame frame = nullptr;
    mpp_frame_init(&frame);
    mpp_frame_set_width(frame, m_width);
    mpp_frame_set_height(frame, m_height);
    mpp_frame_set_hor_stride(frame, m_width * 2);
    mpp_frame_set_ver_stride(frame, m_height);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV422_YUYV);
    mpp_frame_set_buffer(frame, static_cast<MppBuffer>(it->second.buffer));
    mpp_frame_set_eos(frame, 0);

    MppApi *mpi = api(m_mpi);
    MPP_RET ret = mpi->encode_put_frame(ctx(m_ctx), frame);
    mpp_frame_deinit(&frame);
    if (ret != MPP_OK) {
        std::cerr << "[mpp] encode_put_frame failed: " << ret << std::endl;
        close();
        return m_stream;
    }

    // One packet per frame, unless the encoder splits it into partitions.
    for (;;) {
        MppPacket packet = nullptr;
        ret = mpi->encode_get_packet(ctx(m_ctx), &packet);
        if (ret != MPP_OK || !packet) {
            std::cerr << "[mpp] encode_get_packet failed: " << ret << std::endl;
            m_stream.clear();
            close();
            return m_stream;
        }
        const uint8_t *data = static_cast<const uint8_t *>(mpp_packet_get_pos(packet));
        m_stream.insert(m_stream.end(), data, data + mpp_packet_get_length(packet));
        const bool done = !mpp_packet_is_partition(packet) || mpp_packet_is_eoi(packet);
        mpp_packet_deinit(&packet);
        if (done)
            break;
    }
    return m_stream;
}
