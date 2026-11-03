// ISP tuning service.
//
// Just enough image processing for the Rockchip ISP (RK356x, ISP v2.1, vendor kernel) to get
// natural colors and a usable exposure without Rockchip's rkaiq:
// - fixed: black level, color correction matrix and gamma, from libcamera's tuning of the
//   IMX290 family (the IMX462 is register compatible),
// - a few times a second from the ISP's 15x15 raw AE statistics: gray world white balance
//   (ISP gains) and auto exposure (sensor exposure and analogue gain).
// Every params buffer carries the complete configuration.
//
// The ISP only measures statistics when they are configured at its stream start, so this
// service has to be ready before camera-service starts streaming: it tells systemd
// (Type=notify, ordered before camera-service), and exits as soon as the statistics stop
// (ISP stopped), to be ready again for the next start. Started while the ISP already runs,
// it still applies the fixed settings with a daylight white balance.

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <linux/rk-isp21-config.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>

namespace {

// Black level of the IMX290 family, 60 at 10 bits; the ISP works on 12 bits.
const int BLACK_LEVEL_12BIT = 240;

// libcamera imx290.json, rpi.ccm at 3900 K: rows R, G, B of camera RGB -> sRGB.
const float CCM[3][3] = {
    {1.54659f, -0.17707f, -0.36953f},
    {-0.51471f, 1.72733f, -0.21262f},
    {0.06667f, -0.92279f, 1.85612f},
};

// libcamera imx290.json, rpi.contrast gamma_curve: (x, y) pairs, 16 bit.
const int GAMMA_CURVE[][2] = {
    {0, 0}, {1024, 5040}, {2048, 9338}, {3072, 12356}, {4096, 15312}, {5120, 18051},
    {6144, 20790}, {7168, 23193}, {8192, 25744}, {9216, 27942}, {10240, 30035},
    {11264, 32005}, {12288, 33975}, {13312, 35815}, {14336, 37600}, {15360, 39168},
    {16384, 40642}, {18432, 43379}, {20480, 45749}, {22528, 47753}, {24576, 49621},
    {26624, 51253}, {28672, 52698}, {30720, 53796}, {32768, 54876}, {36864, 57012},
    {40960, 58656}, {45056, 59954}, {49152, 61183}, {53248, 62355}, {57344, 63419},
    {61440, 64476}, {65535, 65535},
};

// Input positions (12 bit) of the 45 ISP gamma points, logarithmic segments (equ_segm 0).
const int GAMMA_X[ISP2X_GAMMA_OUT_MAX_SAMPLES] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96,
    112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 640, 768, 896, 1024, 1280, 1536,
    1792, 2048, 2560, 3072, 3584, 4095,
};

const uint64_t MODULES = ISP2X_MODULE_BLS | ISP2X_MODULE_AWB_GAIN | ISP2X_MODULE_CCM |
                         ISP2X_MODULE_GOC | ISP2X_MODULE_RAWAE3;

// Statistics come with every frame (16 ms at 60 fps), a longer gap means the ISP stopped.
const int STATS_TIMEOUT_MS = 500;
const int NUM_BUFFERS = 4;

struct Settings {
    int update_every = 6;       // frames between updates, 10 per second at 60 fps
    float ae_target = 0.16f;    // mean linear level (0..1) after black level, like libcamera
    int max_gain = 3200;        // analogue_gain units, 100 = 1x
    float fixed_wb_r = 0.0f;    // > 0: fixed gains instead of gray world
    float fixed_wb_b = 0.0f;
    int fixed_exposure = 0;     // > 0: fixed exposure and gain instead of auto exposure
    int fixed_gain = 0;
    bool verbose = false;
};

struct State {
    float wb_r = 1.6f, wb_b = 1.6f;  // a daylight guess until the first statistics
    int exposure = 0, gain = 0;
};

/// sd_notify(READY=1) without libsystemd.
void notifyReady() {
    const char *path = getenv("NOTIFY_SOCKET");
    if (!path || !*path)
        return;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (addr.sun_path[0] == '@') // abstract socket
        addr.sun_path[0] = 0;
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    const char msg[] = "READY=1";
    sendto(fd, msg, sizeof(msg) - 1, 0, reinterpret_cast<sockaddr *>(&addr),
           offsetof(sockaddr_un, sun_path) + std::strlen(path));
    close(fd);
}

struct Buffer {
    void *data = nullptr;
    size_t length = 0;
};

int xioctl(int fd, unsigned long request, void *arg) {
    int ret;
    do {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);
    return ret;
}

/// /dev/videoN whose V4L2 name is @p name, empty if none.
std::string findVideoNode(const std::string &name) {
    for (int i = 0; i < 64; i++) {
        std::ifstream f("/sys/class/video4linux/video" + std::to_string(i) + "/name");
        std::string n;
        if (f && std::getline(f, n) && n == name)
            return "/dev/video" + std::to_string(i);
    }
    return "";
}

bool hasControl(int fd, uint32_t id, v4l2_queryctrl *qc) {
    std::memset(qc, 0, sizeof(*qc));
    qc->id = id;
    return xioctl(fd, VIDIOC_QUERYCTRL, qc) == 0 && !(qc->flags & V4L2_CTRL_FLAG_DISABLED);
}

int getControl(int fd, uint32_t id) {
    v4l2_control c{id, 0};
    return xioctl(fd, VIDIOC_G_CTRL, &c) == 0 ? c.value : -1;
}

bool setControl(int fd, uint32_t id, int value) {
    v4l2_control c{id, value};
    return xioctl(fd, VIDIOC_S_CTRL, &c) == 0;
}

/// Opens the sensor subdev: the one with exposure and analogue gain controls.
int openSensor() {
    for (int i = 0; i < 16; i++) {
        std::string path = "/dev/v4l-subdev" + std::to_string(i);
        int fd = open(path.c_str(), O_RDWR);
        if (fd < 0)
            continue;
        v4l2_queryctrl qc;
        if (hasControl(fd, V4L2_CID_EXPOSURE, &qc) && hasControl(fd, V4L2_CID_ANALOGUE_GAIN, &qc)) {
            std::cout << "Sensor " << path << std::endl;
            return fd;
        }
        close(fd);
    }
    return -1;
}

bool setupQueue(int fd, v4l2_buf_type type, std::vector<Buffer> &buffers) {
    v4l2_requestbuffers req{};
    req.count = NUM_BUFFERS;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count == 0)
        return false;
    buffers.resize(req.count);
    for (unsigned i = 0; i < req.count; i++) {
        v4l2_buffer buf{};
        buf.type = type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0)
            return false;
        buffers[i].length = buf.length;
        buffers[i].data = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (buffers[i].data == MAP_FAILED)
            return false;
    }
    return true;
}

bool queueBuffer(int fd, v4l2_buf_type type, unsigned index, size_t bytes = 0) {
    v4l2_buffer buf{};
    buf.type = type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    buf.bytesused = bytes;
    return xioctl(fd, VIDIOC_QBUF, &buf) == 0;
}

/// Interpolated libcamera gamma at 16 bit input @p x.
int gamma16(int x) {
    const int n = sizeof(GAMMA_CURVE) / sizeof(GAMMA_CURVE[0]);
    for (int i = 1; i < n; i++) {
        if (x <= GAMMA_CURVE[i][0]) {
            const int x0 = GAMMA_CURVE[i - 1][0], y0 = GAMMA_CURVE[i - 1][1];
            const int x1 = GAMMA_CURVE[i][0], y1 = GAMMA_CURVE[i][1];
            return y0 + (int64_t)(y1 - y0) * (x - x0) / (x1 - x0);
        }
    }
    return 65535;
}

int16_t ccmCoeff(float c) {
    return (int16_t)std::lround(c * 128.0f); // 7 fractional bits
}

uint16_t wbGain(float g) {
    return (uint16_t)std::lround(std::clamp(g, 0.25f, 15.9f) * 256.0f); // 8 fractional bits
}

/// The complete ISP configuration for the current white balance.
void fillParams(isp21_isp_params_cfg *p, const State &s, int width, int height, uint32_t frame_id) {
    std::memset(p, 0, sizeof(*p));
    p->module_en_update = MODULES;
    p->module_ens = MODULES;
    p->module_cfg_update = MODULES;
    p->frame_id = frame_id;

    isp21_bls_cfg &bls = p->others.bls_cfg;
    bls.fixed_val = {BLACK_LEVEL_12BIT, BLACK_LEVEL_12BIT, BLACK_LEVEL_12BIT, BLACK_LEVEL_12BIT};

    isp21_awb_gain_cfg &awb = p->others.awb_gain_cfg;
    awb.gain0_red = awb.gain1_red = awb.gain2_red = wbGain(s.wb_r);
    awb.gain0_blue = awb.gain1_blue = awb.gain2_blue = wbGain(s.wb_b);
    awb.gain0_green_r = awb.gain1_green_r = awb.gain2_green_r = wbGain(1.0f);
    awb.gain0_green_b = awb.gain1_green_b = awb.gain2_green_b = wbGain(1.0f);

    isp21_ccm_cfg &ccm = p->others.ccm_cfg;
    ccm.highy_adjust_dis = 1;
    ccm.bound_bit = 0x0a;
    ccm.coeff0_r = ccmCoeff(CCM[0][0]); ccm.coeff1_r = ccmCoeff(CCM[0][1]); ccm.coeff2_r = ccmCoeff(CCM[0][2]);
    ccm.coeff0_g = ccmCoeff(CCM[1][0]); ccm.coeff1_g = ccmCoeff(CCM[1][1]); ccm.coeff2_g = ccmCoeff(CCM[1][2]);
    ccm.coeff0_b = ccmCoeff(CCM[2][0]); ccm.coeff1_b = ccmCoeff(CCM[2][1]); ccm.coeff2_b = ccmCoeff(CCM[2][2]);
    ccm.coeff0_y = 38; ccm.coeff1_y = 75; ccm.coeff2_y = 15; // RGB -> Y, 7 fractional bits

    isp2x_gammaout_cfg &goc = p->others.gammaout_cfg;
    goc.equ_segm = 0;
    goc.offset = 0;
    for (int i = 0; i < ISP2X_GAMMA_OUT_MAX_SAMPLES; i++)
        goc.gamma_y[i] = std::min(4095, gamma16(GAMMA_X[i] * 65535 / 4095) * 4095 / 65535);

    // The big raw AE block, the lite one (rawae0) only measures in HDR modes. Input 0: after the
    // black level and the white balance gains.
    isp2x_rawaebig_meas_cfg &ae = p->meas.rawae3;
    ae.rawae_sel = 0;
    ae.wnd_num = 2; // 15x15 blocks
    ae.win = {0, 0, (uint16_t)width, (uint16_t)height};
}

} // namespace

int main(int argc, char **argv) {
    Settings cfg;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--update-every" && i + 1 < argc) {
            cfg.update_every = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--ae-target" && i + 1 < argc) {
            cfg.ae_target = std::atof(argv[++i]);
        } else if (arg == "--max-gain" && i + 1 < argc) {
            cfg.max_gain = std::atoi(argv[++i]);
        } else if (arg == "--wb" && i + 2 < argc) {
            cfg.fixed_wb_r = std::atof(argv[++i]);
            cfg.fixed_wb_b = std::atof(argv[++i]);
        } else if (arg == "--exposure" && i + 2 < argc) {
            cfg.fixed_exposure = std::atoi(argv[++i]);
            cfg.fixed_gain = std::atoi(argv[++i]);
        } else if (arg == "--verbose") {
            cfg.verbose = true;
        } else {
            std::cout << "Usage: " << argv[0] << " [--update-every 6] [--ae-target 0.16] [--max-gain 3200]"
                      << " [--wb <red> <blue>] [--exposure <lines> <gain>] [--verbose]" << std::endl;
            return 1;
        }
    }

    const std::string params_path = findVideoNode("rkisp-input-params");
    const std::string stats_path = findVideoNode("rkisp-statistics");
    if (params_path.empty() || stats_path.empty()) {
        std::cout << "No Rockchip ISP params / statistics nodes, nothing to do" << std::endl;
        return 0;
    }
    const int params_fd = open(params_path.c_str(), O_RDWR | O_NONBLOCK);
    const int stats_fd = open(stats_path.c_str(), O_RDWR | O_NONBLOCK);
    const int sensor_fd = openSensor();
    if (params_fd < 0 || stats_fd < 0 || sensor_fd < 0) {
        std::cout << "Can not open " << params_path << ", " << stats_path << " or the sensor" << std::endl;
        return 1;
    }

    // Sensor limits. Exposure in lines, kept within the frame so the frame rate stays.
    v4l2_subdev_format fmt{};
    fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    xioctl(sensor_fd, VIDIOC_SUBDEV_G_FMT, &fmt);
    const int width = fmt.format.width, height = fmt.format.height;
    v4l2_queryctrl qexp, qgain, qvblank;
    hasControl(sensor_fd, V4L2_CID_EXPOSURE, &qexp);
    hasControl(sensor_fd, V4L2_CID_ANALOGUE_GAIN, &qgain);
    const int vblank = hasControl(sensor_fd, V4L2_CID_VBLANK, &qvblank) ? getControl(sensor_fd, V4L2_CID_VBLANK) : 0;
    const int max_exposure = std::min(qexp.maximum, height + vblank - 4);
    const int min_gain = qgain.minimum, max_gain = std::min(qgain.maximum, cfg.max_gain);
    std::cout << "Frame " << width << "x" << height << ", exposure " << qexp.minimum << ".." << max_exposure
              << " lines, gain " << min_gain << ".." << max_gain << std::endl;

    State state;
    state.exposure = getControl(sensor_fd, V4L2_CID_EXPOSURE);
    state.gain = getControl(sensor_fd, V4L2_CID_ANALOGUE_GAIN);
    if (cfg.fixed_wb_r > 0) {
        state.wb_r = cfg.fixed_wb_r;
        state.wb_b = cfg.fixed_wb_b;
    }
    if (cfg.fixed_exposure > 0) {
        state.exposure = std::clamp(cfg.fixed_exposure, qexp.minimum, max_exposure);
        state.gain = std::clamp(cfg.fixed_gain, min_gain, qgain.maximum);
        setControl(sensor_fd, V4L2_CID_EXPOSURE, state.exposure);
        setControl(sensor_fd, V4L2_CID_ANALOGUE_GAIN, state.gain);
    }

    std::vector<Buffer> params_bufs, stats_bufs;
    if (!setupQueue(params_fd, V4L2_BUF_TYPE_META_OUTPUT, params_bufs) ||
        !setupQueue(stats_fd, V4L2_BUF_TYPE_META_CAPTURE, stats_bufs)) {
        std::cout << "Can not set up the ISP buffers" << std::endl;
        return 1;
    }
    for (unsigned i = 0; i < stats_bufs.size(); i++)
        queueBuffer(stats_fd, V4L2_BUF_TYPE_META_CAPTURE, i);
    v4l2_buf_type stats_type = V4L2_BUF_TYPE_META_CAPTURE, params_type = V4L2_BUF_TYPE_META_OUTPUT;
    if (xioctl(stats_fd, VIDIOC_STREAMON, &stats_type) < 0 || xioctl(params_fd, VIDIOC_STREAMON, &params_type) < 0) {
        std::cout << "Can not start the ISP params / statistics: " << strerror(errno) << std::endl;
        return 1;
    }

    // Params buffers the driver gave back. The first one queued after REQBUFS is only kept
    // for the next ISP start, so the configuration is sent twice at the beginning.
    std::vector<unsigned> free_params;
    for (unsigned i = 0; i < params_bufs.size(); i++)
        free_params.push_back(i);
    auto sendParams = [&](uint32_t frame_id) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_META_OUTPUT;
        buf.memory = V4L2_MEMORY_MMAP;
        while (xioctl(params_fd, VIDIOC_DQBUF, &buf) == 0)
            free_params.push_back(buf.index);
        if (free_params.empty())
            return;
        const unsigned index = free_params.back();
        free_params.pop_back();
        auto *p = static_cast<isp21_isp_params_cfg *>(params_bufs[index].data);
        fillParams(p, state, width, height, frame_id);
        // The driver sizes the buffers for the (smaller) ISP v2.0 struct but reads the v2.1 one
        // from them; the mapping is page aligned, so it fits.
        if (!queueBuffer(params_fd, V4L2_BUF_TYPE_META_OUTPUT, index, params_bufs[index].length))
            std::cout << "Can not queue ISP params: " << strerror(errno) << std::endl;
    };
    sendParams(0);
    sendParams(0);
    notifyReady();

    std::cout << "isp-tuning-service running" << std::endl;
    int frames = 0, frames_without_ae = 0;
    bool started = false;
    while (true) {
        // Waits for the ISP to start, then expects statistics with every frame.
        pollfd pfd{stats_fd, POLLIN, 0};
        if (poll(&pfd, 1, started ? STATS_TIMEOUT_MS : -1) <= 0) {
            std::cout << "No ISP statistics in " << STATS_TIMEOUT_MS << " ms, ISP stopped" << std::endl;
            return 1;
        }
        started = true;
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_META_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(stats_fd, VIDIOC_DQBUF, &buf) < 0)
            continue;
        const auto *stat = static_cast<const rkisp_isp21_stat_buffer *>(stats_bufs[buf.index].data);
        const bool have_ae = stat->meas_type & ISP2X_STAT_RAWAE3;
        const uint32_t frame_id = stat->frame_id;

        if (!have_ae) {
            queueBuffer(stats_fd, V4L2_BUF_TYPE_META_CAPTURE, buf.index);
            // ISP started before this service: no statistics until its next start.
            if (++frames_without_ae == 120)
                std::cout << "No AE statistics (ISP started before this service), fixed white balance and exposure"
                          << std::endl;
            if (frames_without_ae % 60 == 0)
                sendParams(frame_id);
            continue;
        }
        if (++frames % cfg.update_every != 0) {
            queueBuffer(stats_fd, V4L2_BUF_TYPE_META_CAPTURE, buf.index);
            continue;
        }

        // Means over the 15x15 blocks, 10 bit scale (G is 12 bit in the statistics), after the
        // black level and the white balance gains.
        double r = 0, g = 0, b = 0;
        int blocks = 0;
        for (const auto &d : stat->params.rawae3.data) {
            const double bg = d.channelg_xy / 4.0;
            if (std::max({double(d.channelr_xy), bg, double(d.channelb_xy)}) > 1000.0)
                continue; // clipped, its color is unknown
            r += d.channelr_xy;
            g += bg;
            b += d.channelb_xy;
            blocks++;
        }
        queueBuffer(stats_fd, V4L2_BUF_TYPE_META_CAPTURE, buf.index);

        if (blocks > 0) {
            r /= blocks;
            g /= blocks;
            b /= blocks;
            const double lr = std::max(1.0, r), lg = std::max(1.0, g), lb = std::max(1.0, b);

            // Gray world: the scene average is neutral. The statistics already include the gains,
            // so they are corrected by what is left, a third of the way per update.
            if (cfg.fixed_wb_r <= 0) {
                state.wb_r = std::clamp(state.wb_r * std::pow(float(lg / lr), 0.3f), 0.5f, 4.0f);
                state.wb_b = std::clamp(state.wb_b * std::pow(float(lg / lb), 0.3f), 0.5f, 4.0f);
            }

            // Auto exposure on green, half way to the target per update (no oscillation with the
            // sensor's 1 to 2 frames delay). Exposure first, gain only above the longest exposure.
            if (cfg.fixed_exposure <= 0) {
                const double level = lg / (1023.0 - BLACK_LEVEL_12BIT / 4.0);
                const double ratio = std::clamp(std::sqrt(cfg.ae_target / std::max(level, 0.001)), 0.5, 2.0);
                const double total = std::clamp(double(state.exposure) * state.gain * ratio,
                                                double(qexp.minimum) * min_gain, double(max_exposure) * max_gain);
                const int exposure = std::clamp(int(total / min_gain), qexp.minimum, max_exposure);
                const int gain = std::clamp(int(total / exposure), min_gain, max_gain);
                if (exposure != state.exposure) {
                    setControl(sensor_fd, V4L2_CID_EXPOSURE, exposure);
                    state.exposure = exposure;
                }
                if (std::abs(gain - state.gain) > state.gain / 50) {
                    setControl(sensor_fd, V4L2_CID_ANALOGUE_GAIN, gain);
                    state.gain = gain;
                }
            }
            if (cfg.verbose)
                std::cout << "frame " << frame_id << " mean R " << r << " G " << g << " B " << b << " (" << blocks
                          << " blocks) -> wb " << state.wb_r << " " << state.wb_b << ", exposure " << state.exposure
                          << " gain " << state.gain << std::endl;
        }
        sendParams(frame_id);
    }
}
