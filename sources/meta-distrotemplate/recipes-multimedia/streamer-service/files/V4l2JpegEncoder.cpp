#include "V4l2JpegEncoder.h"

#include <cerrno>
#include <cstring>
#include <iostream>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

const uint32_t OUTPUT_BUFFERS = 8;
const uint32_t CAPTURE_BUFFERS = 4;
const int ENCODE_TIMEOUT_MS = 1000;

int xioctl(int fd, unsigned long request, void *arg) {
    int ret;
    do {
        ret = ::ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);
    return ret;
}

/// mem2mem device with JPEG on its CAPTURE (coded) side.
bool isJpegEncoder(int fd, std::string &card) {
    v4l2_capability cap{};
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0)
        return false;
    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_M2M_MPLANE) || !(caps & V4L2_CAP_STREAMING))
        return false;

    v4l2_fmtdesc desc{};
    desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    for (desc.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &desc) == 0; desc.index++) {
        if (desc.pixelformat == V4L2_PIX_FMT_JPEG) {
            card = reinterpret_cast<const char *>(cap.card);
            return true;
        }
    }
    return false;
}

} // namespace

V4l2JpegEncoder::~V4l2JpegEncoder() {
    close();
}

bool V4l2JpegEncoder::open(int width, int height) {
    const std::string &device = m_device;
    const int quality = m_quality;
    close();

    std::string path, card;
    if (!device.empty()) {
        m_fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK);
        if (m_fd < 0 || !isJpegEncoder(m_fd, card)) {
            std::cerr << "[encoder] " << device << " is not a JPEG encoder" << std::endl;
            close();
            return false;
        }
        path = device;
    } else {
        for (int i = 0; i < 64 && m_fd < 0; i++) {
            std::string candidate = "/dev/video" + std::to_string(i);
            int fd = ::open(candidate.c_str(), O_RDWR | O_NONBLOCK);
            if (fd < 0)
                continue;
            if (isJpegEncoder(fd, card)) {
                m_fd = fd;
                path = candidate;
            } else {
                ::close(fd);
            }
        }
        if (m_fd < 0) {
            std::cerr << "[encoder] no V4L2 JPEG encoder found" << std::endl;
            return false;
        }
    }

    // Coded format first, for Hantro it resets the raw format.
    v4l2_format coded{};
    coded.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    coded.fmt.pix_mp.width = width;
    coded.fmt.pix_mp.height = height;
    coded.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_JPEG;
    coded.fmt.pix_mp.field = V4L2_FIELD_NONE;
    coded.fmt.pix_mp.num_planes = 1;
    if (xioctl(m_fd, VIDIOC_S_FMT, &coded) < 0) {
        std::cerr << "[encoder] S_FMT JPEG failed: " << strerror(errno) << std::endl;
        close();
        return false;
    }

    v4l2_format raw{};
    raw.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    raw.fmt.pix_mp.width = width;
    raw.fmt.pix_mp.height = height;
    raw.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUYV;
    raw.fmt.pix_mp.field = V4L2_FIELD_NONE;
    raw.fmt.pix_mp.num_planes = 1;
    raw.fmt.pix_mp.plane_fmt[0].bytesperline = width * 2;
    if (xioctl(m_fd, VIDIOC_S_FMT, &raw) < 0) {
        std::cerr << "[encoder] S_FMT YUYV failed: " << strerror(errno) << std::endl;
        close();
        return false;
    }
    // Input buffers are tightly packed, the driver must not change the layout.
    if ((int)raw.fmt.pix_mp.width != width || (int)raw.fmt.pix_mp.height != height ||
        raw.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUYV ||
        (int)raw.fmt.pix_mp.plane_fmt[0].bytesperline != width * 2) {
        std::cerr << "[encoder] driver wants " << raw.fmt.pix_mp.width << "x" << raw.fmt.pix_mp.height
                  << " stride " << raw.fmt.pix_mp.plane_fmt[0].bytesperline << " for " << width << "x"
                  << height << " YUYV" << std::endl;
        close();
        return false;
    }
    m_raw_size = raw.fmt.pix_mp.plane_fmt[0].sizeimage;

    v4l2_control ctrl{};
    ctrl.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
    ctrl.value = quality;
    if (xioctl(m_fd, VIDIOC_S_CTRL, &ctrl) < 0)
        std::cerr << "[encoder] can not set quality: " << strerror(errno) << std::endl;

    // Raw frames are imported dma-bufs, JPEG goes to driver allocated buffers.
    v4l2_requestbuffers req{};
    req.count = OUTPUT_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_DMABUF;
    if (xioctl(m_fd, VIDIOC_REQBUFS, &req) < 0) {
        std::cerr << "[encoder] REQBUFS output failed: " << strerror(errno) << std::endl;
        close();
        return false;
    }

    req = {};
    req.count = CAPTURE_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(m_fd, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) {
        std::cerr << "[encoder] REQBUFS capture failed: " << strerror(errno) << std::endl;
        close();
        return false;
    }

    for (uint32_t i = 0; i < req.count; i++) {
        v4l2_plane plane{};
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = &plane;
        buf.length = 1;
        if (xioctl(m_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            std::cerr << "[encoder] QUERYBUF failed: " << strerror(errno) << std::endl;
            close();
            return false;
        }
        void *data = mmap(nullptr, plane.length, PROT_READ, MAP_SHARED, m_fd, plane.m.mem_offset);
        if (data == MAP_FAILED) {
            std::cerr << "[encoder] mmap failed: " << strerror(errno) << std::endl;
            close();
            return false;
        }
        m_capture.push_back({data, plane.length});
        if (xioctl(m_fd, VIDIOC_QBUF, &buf) < 0) {
            std::cerr << "[encoder] QBUF capture failed: " << strerror(errno) << std::endl;
            close();
            return false;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(m_fd, VIDIOC_STREAMON, &type) < 0) {
        std::cerr << "[encoder] STREAMON output failed: " << strerror(errno) << std::endl;
        close();
        return false;
    }
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(m_fd, VIDIOC_STREAMON, &type) < 0) {
        std::cerr << "[encoder] STREAMON capture failed: " << strerror(errno) << std::endl;
        close();
        return false;
    }

    m_width = width;
    m_height = height;
    std::cout << "[encoder] " << card << " (" << path << ") " << width << "x" << height
              << " YUYV -> JPEG, quality " << quality << std::endl;
    return true;
}

void V4l2JpegEncoder::close() {
    if (m_fd < 0)
        return;
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    xioctl(m_fd, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(m_fd, VIDIOC_STREAMOFF, &type);
    for (auto &buffer : m_capture)
        munmap(buffer.data, buffer.size);
    m_capture.clear();
    m_slots.clear();
    ::close(m_fd);
    m_fd = -1;
    m_width = m_height = 0;
}

const std::vector<uint8_t> &V4l2JpegEncoder::encode(int dma_buf_fd, uint32_t length) {
    m_jpeg.clear();
    if (m_fd < 0)
        return m_jpeg;

    struct stat st;
    if (fstat(dma_buf_fd, &st) < 0 || length < m_raw_size) {
        std::cerr << "[encoder] invalid input frame, length " << length << " < " << m_raw_size << std::endl;
        return m_jpeg;
    }

    auto slot = m_slots.find(st.st_ino);
    if (slot == m_slots.end()) {
        if (m_slots.size() >= OUTPUT_BUFFERS)
            m_slots.clear();
        slot = m_slots.emplace(st.st_ino, m_slots.size()).first;
    }

    v4l2_plane in_plane{};
    in_plane.m.fd = dma_buf_fd;
    in_plane.length = length;
    in_plane.bytesused = m_raw_size;
    v4l2_buffer in{};
    in.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    in.memory = V4L2_MEMORY_DMABUF;
    in.index = slot->second;
    in.field = V4L2_FIELD_NONE;
    in.m.planes = &in_plane;
    in.length = 1;
    if (xioctl(m_fd, VIDIOC_QBUF, &in) < 0) {
        std::cerr << "[encoder] QBUF input failed: " << strerror(errno) << std::endl;
        close();
        return m_jpeg;
    }

    pollfd pfd{m_fd, POLLIN, 0};
    if (::poll(&pfd, 1, ENCODE_TIMEOUT_MS) <= 0) {
        std::cerr << "[encoder] timeout" << std::endl;
        close();
        return m_jpeg;
    }

    v4l2_plane out_plane{};
    v4l2_buffer out{};
    out.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    out.memory = V4L2_MEMORY_MMAP;
    out.m.planes = &out_plane;
    out.length = 1;
    if (xioctl(m_fd, VIDIOC_DQBUF, &out) < 0) {
        std::cerr << "[encoder] DQBUF JPEG failed: " << strerror(errno) << std::endl;
        close();
        return m_jpeg;
    }

    // The input buffer is released together with the encoded one.
    v4l2_plane done_plane{};
    v4l2_buffer done{};
    done.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    done.memory = V4L2_MEMORY_DMABUF;
    done.m.planes = &done_plane;
    done.length = 1;
    pollfd out_pfd{m_fd, POLLOUT, 0};
    if (::poll(&out_pfd, 1, ENCODE_TIMEOUT_MS) <= 0 || xioctl(m_fd, VIDIOC_DQBUF, &done) < 0) {
        std::cerr << "[encoder] DQBUF input failed: " << strerror(errno) << std::endl;
        close();
        return m_jpeg;
    }

    if (out.flags & V4L2_BUF_FLAG_ERROR) {
        std::cerr << "[encoder] encode error" << std::endl;
    } else if (out.index < m_capture.size() && out_plane.bytesused <= m_capture[out.index].size) {
        const uint8_t *data = static_cast<const uint8_t *>(m_capture[out.index].data);
        m_jpeg.assign(data + out_plane.data_offset, data + out_plane.bytesused);
    }

    out_plane.bytesused = 0;
    if (xioctl(m_fd, VIDIOC_QBUF, &out) < 0) {
        // This frame is fine, the encoder is reopened for the next one.
        std::cerr << "[encoder] QBUF JPEG failed: " << strerror(errno) << std::endl;
        close();
    }
    return m_jpeg;
}
