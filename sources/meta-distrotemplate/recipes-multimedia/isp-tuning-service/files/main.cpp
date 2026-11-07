// ISP tuning service.
//
// Just enough image processing for the Rockchip ISP (RK356x, ISP v2.1, vendor kernel) to get
// natural colors and a usable exposure without Rockchip's rkaiq:
// - from libcamera's tuning of the sensor (picked by the sensor subdev name: IMX219, else the
//   IMX290 family, which the IMX462 is register compatible with): black level, gamma and the
//   color correction matrix, for the IMX219 interpolated by the color temperature the white
//   balance implies,
// - IMX219: defect pixel correction, luma and chroma denoise, sharpening and a little more
//   saturation, from Rockchip's rkaiq tunings, following the sensor gain (more denoise and less
//   sharpening as the noise grows). Generic, not tuned to a scene; --plain switches them off,
// - a few times a second from the ISP's 15x15 raw AE statistics: gray world white balance over
//   the near neutral blocks (ISP gains) and auto exposure (sensor exposure and analogue gain).
// Every params buffer carries the complete configuration.
//
// The ISP only measures statistics when they are configured at its stream start, so this
// service has to be ready before camera-service starts streaming: it tells systemd
// (Type=notify, ordered before camera-service), and exits as soon as the statistics stop
// (ISP stopped), to be ready again for the next start. Started while the ISP already runs,
// it still applies the fixed settings with a daylight white balance.

#include <algorithm>
#include <climits>
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

// The vendor uapi header uses the kernel's BIT_ULL for the ISP 2.1 module bits.
#ifndef BIT_ULL
#define BIT_ULL(n) (1ULL << (n))
#endif
#include <linux/rk-isp21-config.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>

namespace {

/// Color correction matrix at a color temperature: rows R, G, B of camera RGB -> sRGB.
struct CtCcm {
    float ct;
    float m[3][3];
};

/// A point of the sensor's white curve: color temperature and the R/G, B/G of a gray there.
struct CtPoint {
    float ct, r, b;
};

struct SensorTuning {
    const char *name;      // part of the sensor subdev name, empty: any sensor
    int black_level_12bit; // the ISP works on 12 bits
    std::vector<CtCcm> ccms;        // by color temperature, interpolated; one: fixed
    std::vector<CtPoint> ct_curve;  // empty: no color temperature estimate (fixed matrix)
    int gain_unit;                  // analogue_gain control value of 1x
    bool enhance;                   // defect pixels, denoise, sharpen, saturation (see fillEnhance)
};

const SensorTuning TUNINGS[] = {
    // libcamera imx219.json: black level 4096 at 16 bits (64 at 10), rpi.ccm and rpi.awb ct_curve.
    {"imx219", 256,
     {{2860, {{2.12089f, -0.52461f, -0.59629f}, {-0.85342f, 2.80445f, -0.95103f}, {-0.26897f, -1.14788f, 2.41685f}}},
      {2960, {{2.26962f, -0.54174f, -0.72789f}, {-0.77008f, 2.60271f, -0.83262f}, {-0.26036f, -1.51254f, 2.77289f}}},
      {3603, {{2.18644f, -0.66148f, -0.52496f}, {-0.77828f, 2.69474f, -0.91645f}, {-0.25239f, -0.83059f, 2.08298f}}},
      {4650, {{2.18174f, -0.70887f, -0.47287f}, {-0.70196f, 2.76426f, -1.06231f}, {-0.25157f, -0.71978f, 1.97135f}}},
      {5858, {{2.32392f, -0.88421f, -0.43971f}, {-0.63821f, 2.58348f, -0.94527f}, {-0.28541f, -0.54112f, 1.82653f}}},
      {7580, {{2.21175f, -0.53242f, -0.67933f}, {-0.57875f, 3.07922f, -1.50047f}, {-0.27709f, -0.73338f, 2.01048f}}}},
     {{2498, 0.9309f, 0.3599f}, {2911, 0.8682f, 0.4283f}, {2919, 0.8358f, 0.4621f}, {3627, 0.7646f, 0.5327f},
      {4600, 0.6079f, 0.6721f}, {5716, 0.5712f, 0.7017f}, {8575, 0.4331f, 0.8037f}},
     256, true},
    // libcamera imx290.json (IMX462 Pivariety): black level 60 at 10 bits, rpi.ccm at 3900 K.
    {"", 240,
     {{3900, {{1.54659f, -0.17707f, -0.36953f}, {-0.51471f, 1.72733f, -0.21262f}, {0.06667f, -0.92279f, 1.85612f}}}},
     {}, 100, false},
};

/// Color temperature of the white point with gains @p wb_r, @p wb_b: the nearest point of the
/// white curve (the gray's R/G = 1 / wb_r, B/G = 1 / wb_b). 0 without a curve.
float estimateCt(const SensorTuning &t, float wb_r, float wb_b) {
    const float r = 1.0f / wb_r, b = 1.0f / wb_b;
    float best_ct = 0, best_d = 1e9f;
    for (size_t i = 1; i < t.ct_curve.size(); i++) {
        const CtPoint &p0 = t.ct_curve[i - 1], &p1 = t.ct_curve[i];
        const float dr = p1.r - p0.r, db = p1.b - p0.b;
        const float k = std::clamp(((r - p0.r) * dr + (b - p0.b) * db) / (dr * dr + db * db), 0.0f, 1.0f);
        const float er = p0.r + k * dr - r, eb = p0.b + k * db - b;
        if (er * er + eb * eb < best_d) {
            best_d = er * er + eb * eb;
            best_ct = p0.ct + k * (p1.ct - p0.ct);
        }
    }
    return best_ct;
}

/// The color matrix at @p ct, linear between the calibrated ones.
void ccmAt(const SensorTuning &t, float ct, float m[3][3]) {
    const auto &c = t.ccms;
    size_t i = 1;
    while (i < c.size() - 1 && ct > c[i].ct)
        i++;
    const float k = c.size() < 2 ? 0.0f : std::clamp((ct - c[i - 1].ct) / (c[i].ct - c[i - 1].ct), 0.0f, 1.0f);
    const CtCcm &a = c[c.size() < 2 ? 0 : i - 1], &b = c[c.size() < 2 ? 0 : i];
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 3; x++)
            m[y][x] = a.m[y][x] + k * (b.m[y][x] - a.m[y][x]);
}

// libcamera rpi.contrast gamma_curve, the same in imx290.json and imx219.json: (x, y) pairs, 16 bit.
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
const uint64_t MODULES_ENHANCE = ISP2X_MODULE_DPCC | ISP2X_MODULE_DEBAYER | ISP2X_MODULE_YNR |
                                 ISP2X_MODULE_CNR | ISP2X_MODULE_SHARP | ISP2X_MODULE_CPROC;

// White balance: blocks within this log ratio of gray (after the current gains) count as
// neutral, about +-35 %.
const double AWB_NEUTRAL_LOG = 0.3;

// Statistics come with every frame (16 ms at 60 fps), a longer gap means the ISP stopped.
const int STATS_TIMEOUT_MS = 500;
const int NUM_BUFFERS = 4;

struct Settings {
    int update_every = 6;       // frames between updates, 10 per second at 60 fps
    float ae_target = 0.16f;    // mean linear level (0..1) after black level, like libcamera
    int max_gain = 3200;        // the sensor's analogue_gain units (IMX462 100 = 1x, IMX219 256 = 1x)
    float fixed_wb_r = 0.0f;    // > 0: fixed gains instead of gray world
    float fixed_wb_b = 0.0f;
    int fixed_exposure = 0;     // > 0: fixed exposure and gain instead of auto exposure
    int fixed_gain = 0;
    bool verbose = false;
    bool enhance = true;        // false: no defect pixel, denoise, sharpen, saturation stages
    int sharpen_low = 4;        // sharpening strength x4 at low gain (2x and less), 1x
    int sharpen_high = 0;       // and at high gain (32x): none, it would sharpen noise
    int saturation = 140;       // at low gain, 128 = 1 (neutral at high gain)
    float denoise_low = 0.5f;   // luma denoise strength (noise curve scale) at low gain
    float denoise_high = 2.0f;  // and at high gain
    uint64_t disabled = 0;      // stages switched off with --disable
};

struct State {
    float wb_r = 1.6f, wb_b = 1.6f;  // a daylight guess until the first statistics
    float ct = 0;                     // estimated color temperature, 0: unknown
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

/// Opens the sensor subdev: one with exposure and analogue gain controls, preferably an I2C
/// device (the Rockchip ISP and CSI subdevs inherit the sensor's controls, but not its name).
/// @p name: its V4L2 name, e.g. "m00_b_imx219 2-0010".
int openSensor(std::string *name) {
    int found = -1;
    for (int pass = 0; pass < 2 && found < 0; pass++) {
        for (int i = 0; i < 16; i++) {
            const std::string sys = "/sys/class/video4linux/v4l-subdev" + std::to_string(i);
            char dev[PATH_MAX];
            const bool i2c = realpath((sys + "/device").c_str(), dev) && std::strstr(dev, "/i2c-");
            if (pass == 0 && !i2c)
                continue;
            const std::string path = "/dev/v4l-subdev" + std::to_string(i);
            int fd = open(path.c_str(), O_RDWR);
            if (fd < 0)
                continue;
            v4l2_queryctrl qc;
            if (hasControl(fd, V4L2_CID_EXPOSURE, &qc) && hasControl(fd, V4L2_CID_ANALOGUE_GAIN, &qc)) {
                std::ifstream f(sys + "/name");
                std::getline(f, *name);
                std::cout << "Sensor " << path << " (" << *name << ")" << std::endl;
                found = fd;
                break;
            }
            close(fd);
        }
    }
    return found;
}

/// The tuning of the sensor named @p name.
const SensorTuning &findTuning(const std::string &name) {
    for (const auto &t : TUNINGS)
        if (name.find(t.name) != std::string::npos) // the empty name matches any sensor
            return t;
    return TUNINGS[sizeof(TUNINGS) / sizeof(TUNINGS[0]) - 1];
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

/// Linear interpolation, @p k 0..1.
int lerp(int a, int b, float k) {
    return (int)std::lround(a + (b - a) * k);
}

/// Defect pixel correction, demosaic, luma and chroma denoise, sharpening and saturation, from
/// Rockchip's rkaiq for ISP 2.1: its generic template (ov5695) and a calibrated tuning (imx334),
/// converted to these registers. Interpolated by @p k between low gain (0, 2x and less) and
/// high gain (1, 32x): less sharpening and more denoising as the noise grows.
void fillEnhance(isp21_isp_params_cfg *p, const Settings &cfg, float k) {
    // Defect pixels: rkaiq template, set 1 (all methods have to agree: conservative).
    isp2x_dpcc_cfg &dpcc = p->others.dpcc_cfg;
    dpcc.stage1_enable = 1;
    dpcc.sw_rk_out_sel = 1;
    dpcc.sw_dpcc_output_sel = 1;
    dpcc.stage1_incl_rb_center = dpcc.stage1_incl_green_center = 1;
    dpcc.stage1_use_fix_set = 1;
    dpcc.stage1_use_set_1 = 1;
    dpcc.sw_rk_red_blue1_en = dpcc.rg_red_blue1_enable = dpcc.rnd_red_blue1_enable = 1;
    dpcc.ro_red_blue1_enable = dpcc.lc_red_blue1_enable = dpcc.pg_red_blue1_enable = 1;
    dpcc.sw_rk_green1_en = dpcc.rg_green1_enable = dpcc.rnd_green1_enable = 1;
    dpcc.ro_green1_enable = dpcc.lc_green1_enable = dpcc.pg_green1_enable = 1;
    dpcc.line_thr_1_rb = dpcc.line_thr_1_g = 8;
    dpcc.line_mad_fac_1_rb = dpcc.line_mad_fac_1_g = 4;
    dpcc.pg_fac_1_rb = 4;
    dpcc.pg_fac_1_g = 3;
    dpcc.rnd_thr_1_rb = dpcc.rnd_thr_1_g = 10;
    dpcc.rnd_offs_1_rb = dpcc.rnd_offs_1_g = 3;
    dpcc.rg_fac_1_rb = dpcc.rg_fac_1_g = 32;
    dpcc.ro_lim_1_rb = dpcc.ro_lim_1_g = 2;

    // Demosaic (on by default, its filters at reset values otherwise).
    isp2x_debayer_cfg &dm = p->others.debayer_cfg;
    dm.filter_c_en = dm.filter_g_en = 1;
    dm.thed1 = 6;
    dm.thed0 = 3;
    dm.dist_scale = 8;
    dm.max_ratio = 4;
    dm.clip_en = 1;
    dm.filter1_coe1 = 2; dm.filter1_coe2 = -6; dm.filter1_coe3 = 0; dm.filter1_coe4 = 6; dm.filter1_coe5 = -2;
    dm.filter2_coe1 = 2; dm.filter2_coe2 = -4; dm.filter2_coe3 = 4; dm.filter2_coe4 = -4; dm.filter2_coe5 = 2;
    dm.hf_offset = 1;
    dm.gain_offset = 4;
    dm.offset = 1;
    dm.shift_num = 2;
    dm.order_min = 5;
    dm.order_max = 14;

    // Luma denoise: template, noise curves (luma 0..1024 in 17 points) growing with the gain.
    static const uint16_t LSGM[ISP21_YNR_XY_NUM] = {43, 69, 85, 94, 99, 100, 100, 98, 96, 94, 92, 89, 86, 80, 71, 58, 37};
    static const uint16_t HSGM[ISP21_YNR_XY_NUM] = {87, 138, 170, 189, 198, 201, 200, 196, 193, 188, 184, 179, 172, 161, 143, 116, 74};
    isp21_ynr_cfg &ynr = p->others.ynr_cfg;
    ynr.sw_ynr_global_gain_alpha = 8;
    ynr.sw_ynr_global_gain = 16;
    ynr.sw_ynr_rnr_max_r = 7985; // for 1280x720
    ynr.sw_ynr_low_bf_inv0 = lerp(787, 568, k);
    ynr.sw_ynr_low_bf_inv1 = 1024;
    ynr.sw_ynr_low_peak_supress = 64;
    ynr.sw_ynr_low_thred_adj = 16;
    ynr.sw_ynr_low_dist_adj = 32;
    ynr.sw_ynr_low_edge_adj_thresh = 7;
    ynr.sw_ynr_low_bi_weight = 25;
    ynr.sw_ynr_low_weight = 64;
    ynr.sw_ynr_low_center_weight = 409;
    ynr.sw_ynr_hi_min_adj = 57;
    ynr.sw_ynr_high_thred_adj = 64;
    ynr.sw_ynr_high_retain_weight = 29;
    ynr.sw_ynr_hi_edge_thed = 100;
    ynr.sw_ynr_base_filter_weight0 = 18;
    ynr.sw_ynr_base_filter_weight1 = 15;
    ynr.sw_ynr_base_filter_weight2 = 8;
    ynr.sw_ynr_low_gauss1_coeff0 = 11; ynr.sw_ynr_low_gauss1_coeff1 = 31; ynr.sw_ynr_low_gauss1_coeff2 = 88;
    ynr.sw_ynr_low_gauss2_coeff0 = 16; ynr.sw_ynr_low_gauss2_coeff1 = 32; ynr.sw_ynr_low_gauss2_coeff2 = 64;
    ynr.sw_ynr_direction_weight0 = ynr.sw_ynr_direction_weight1 = 16;
    ynr.sw_ynr_direction_weight2 = ynr.sw_ynr_direction_weight3 = 16;
    ynr.sw_ynr_direction_weight4 = ynr.sw_ynr_direction_weight5 = 8;
    ynr.sw_ynr_direction_weight6 = ynr.sw_ynr_direction_weight7 = 8;
    const float sigma_scale = cfg.denoise_low + (cfg.denoise_high - cfg.denoise_low) * k;
    for (int i = 0; i < ISP21_YNR_XY_NUM; i++) {
        ynr.sw_ynr_luma_points_x[i] = i * 64;
        ynr.sw_ynr_lsgm_y[i] = (uint16_t)std::lround(LSGM[i] * sigma_scale);
        ynr.sw_ynr_hsgm_y[i] = (uint16_t)std::lround(HSGM[i] * sigma_scale);
        ynr.sw_ynr_rnr_strength3[i] = 16;
    }

    // Chroma denoise: template at low gain, imx334 at high gain (smaller sigma = stronger).
    isp21_cnr_cfg &cnr = p->others.cnr_cfg;
    cnr.sw_cnr_exgain_bypass = 1;
    cnr.sw_cnr_exgain_mux = 16;
    cnr.sw_cnr_gain_iso = 128;
    cnr.sw_cnr_gain_offset = 12;
    cnr.sw_cnr_gain_1sigma = 40;
    cnr.sw_cnr_gain_uvgain0 = lerp(24, 17, k);
    cnr.sw_cnr_gain_uvgain1 = lerp(64, 32, k);
    cnr.sw_cnr_lmed3_alpha = 3;
    cnr.sw_cnr_lbf5_gain_y = lerp(3, 1, k);
    cnr.sw_cnr_lbf5_gain_c = lerp(14, 2, k);
    cnr.sw_cnr_lbf5_weit_d0 = 128; cnr.sw_cnr_lbf5_weit_d1 = 112; cnr.sw_cnr_lbf5_weit_d2 = 99;
    cnr.sw_cnr_lbf5_weit_d3 = 77; cnr.sw_cnr_lbf5_weit_d4 = 47;
    cnr.sw_cnr_hmed3_alpha = 8;
    cnr.sw_cnr_hbf5_sigma = lerp(1364, 395, k);
    cnr.sw_cnr_lbf5_weit_src = 128;
    cnr.sw_cnr_lbf3_sigma = lerp(3478, 278, k);

    // Sharpening: template filters; strength (x4) between imx334's tuning and the template's.
    static const uint8_t LUMA_DX[ISP21_SHARP_X_NUM] = {6, 6, 7, 7, 8, 8, 7};
    static const uint16_t PBF_INV[2][8] = {{386, 297, 241, 203, 175, 203, 241, 241}, {416, 358, 314, 252, 211, 252, 314, 358}};
    static const uint16_t BF_INV[2][8] = {{482, 410, 356, 315, 282, 315, 356, 356}, {293, 256, 228, 186, 158, 186, 228, 256}};
    // Sharpening amplifies what the edge preserving filters leave as detail: their widest
    // settings (rkaiq's high gain ones) at every gain, and a small overshoot clip, so single
    // noisy pixels don't turn into white points (rkaiq's clip, 64..192, did).
    static const uint16_t CLIP_HF[8] = {16, 24, 32, 40, 48, 40, 32, 0};
    isp21_sharp_cfg &sh = p->others.sharp_cfg;
    sh.sw_sharp_sharp_ratio = std::clamp(lerp(cfg.sharpen_low, cfg.sharpen_high, k), 0, 63);
    sh.sw_sharp_bf_ratio = 128;
    sh.sw_sharp_gaus_ratio = lerp(0, 128, k);
    sh.sw_sharp_pbf_ratio = lerp(0, 26, k);
    for (int i = 0; i < ISP21_SHARP_X_NUM; i++)
        sh.sw_sharp_luma_dx[i] = LUMA_DX[i];
    for (int i = 0; i < ISP21_SHARP_Y_NUM; i++) {
        sh.sw_sharp_pbf_sigma_inv[i] = PBF_INV[1][i];
        sh.sw_sharp_bf_sigma_inv[i] = BF_INV[1][i];
        sh.sw_sharp_ehf_th[i] = lerp(1023, 256, k);
        sh.sw_sharp_clip_hf[i] = CLIP_HF[i];
    }
    sh.sw_sharp_pbf_sigma_shift = 11;
    sh.sw_sharp_bf_sigma_shift = 9;
    sh.sw_sharp_pbf_coef_0 = sh.sw_sharp_bf_coef_0 = sh.sw_sharp_gaus_coef_0 = 24;
    sh.sw_sharp_pbf_coef_1 = sh.sw_sharp_bf_coef_1 = sh.sw_sharp_gaus_coef_1 = 16;
    sh.sw_sharp_pbf_coef_2 = sh.sw_sharp_bf_coef_2 = sh.sw_sharp_gaus_coef_2 = 10;

    // Saturation a little up in good light, neutral when the color noise grows (Q1.7, 128 = 1).
    isp2x_cproc_cfg &cp = p->others.cproc_cfg;
    cp.contrast = 128;
    cp.sat = std::clamp(lerp(cfg.saturation, 128, k), 0, 255);
    cp.brightness = 0;
    cp.hue = 0;
}

/// Gain position between the low (2x) and high (32x) gain settings of fillEnhance, 0..1.
float gainPosition(int gain, int gain_unit) {
    const float g = std::max(1.0f, float(gain) / gain_unit);
    return std::clamp((std::log2(g) - 1.0f) / 4.0f, 0.0f, 1.0f);
}

/// The complete ISP configuration for the current white balance.
void fillParams(isp21_isp_params_cfg *p, const SensorTuning &t, const Settings &cfg, const State &s, int width,
                int height, uint32_t frame_id) {
    std::memset(p, 0, sizeof(*p));
    const bool enhance = t.enhance && cfg.enhance;
    const uint64_t modules = (MODULES | (enhance ? MODULES_ENHANCE : 0)) & ~cfg.disabled;
    p->module_en_update = modules;
    p->module_ens = modules;
    p->module_cfg_update = modules;
    p->frame_id = frame_id;
    if (enhance)
        fillEnhance(p, cfg, gainPosition(s.gain, t.gain_unit));

    isp21_bls_cfg &bls = p->others.bls_cfg;
    const auto bl = (int16_t)t.black_level_12bit;
    bls.fixed_val = {bl, bl, bl, bl};

    isp21_awb_gain_cfg &awb = p->others.awb_gain_cfg;
    awb.gain0_red = awb.gain1_red = awb.gain2_red = wbGain(s.wb_r);
    awb.gain0_blue = awb.gain1_blue = awb.gain2_blue = wbGain(s.wb_b);
    awb.gain0_green_r = awb.gain1_green_r = awb.gain2_green_r = wbGain(1.0f);
    awb.gain0_green_b = awb.gain1_green_b = awb.gain2_green_b = wbGain(1.0f);

    isp21_ccm_cfg &ccm = p->others.ccm_cfg;
    ccm.highy_adjust_dis = 1;
    ccm.bound_bit = 0x0a;
    float m[3][3];
    ccmAt(t, s.ct > 0 ? s.ct : 5000.0f, m);
    ccm.coeff0_r = ccmCoeff(m[0][0]); ccm.coeff1_r = ccmCoeff(m[0][1]); ccm.coeff2_r = ccmCoeff(m[0][2]);
    ccm.coeff0_g = ccmCoeff(m[1][0]); ccm.coeff1_g = ccmCoeff(m[1][1]); ccm.coeff2_g = ccmCoeff(m[1][2]);
    ccm.coeff0_b = ccmCoeff(m[2][0]); ccm.coeff1_b = ccmCoeff(m[2][1]); ccm.coeff2_b = ccmCoeff(m[2][2]);
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
        } else if (arg == "--plain") {
            cfg.enhance = false;
        } else if (arg == "--sharpen" && i + 2 < argc) {
            cfg.sharpen_low = std::atoi(argv[++i]);
            cfg.sharpen_high = std::atoi(argv[++i]);
        } else if (arg == "--denoise" && i + 2 < argc) {
            cfg.denoise_low = std::atof(argv[++i]);
            cfg.denoise_high = std::atof(argv[++i]);
        } else if (arg == "--saturation" && i + 1 < argc) {
            cfg.saturation = std::atoi(argv[++i]);
        } else if (arg == "--disable" && i + 1 < argc) {
            // Comma separated: dpcc, ynr, cnr, sharp, cproc, debayer.
            const std::string list = std::string(",") + argv[++i] + ",";
            const struct { const char *name; uint64_t bit; } STAGES[] = {
                {"dpcc", ISP2X_MODULE_DPCC}, {"ynr", ISP2X_MODULE_YNR}, {"cnr", ISP2X_MODULE_CNR},
                {"sharp", ISP2X_MODULE_SHARP}, {"cproc", ISP2X_MODULE_CPROC}, {"debayer", ISP2X_MODULE_DEBAYER}};
            for (const auto &st : STAGES)
                if (list.find(std::string(",") + st.name + ",") != std::string::npos)
                    cfg.disabled |= st.bit;
        } else if (arg == "--verbose") {
            cfg.verbose = true;
        } else {
            std::cout << "Usage: " << argv[0] << " [--update-every 6] [--ae-target 0.16] [--max-gain 3200]"
                      << " [--wb <red> <blue>] [--exposure <lines> <gain>] [--plain] [--sharpen 4 0]"
                      << " [--denoise 0.5 2.0] [--saturation 140] [--disable dpcc,ynr,cnr,sharp,cproc,debayer]"
                      << " [--verbose]" << std::endl;
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
    std::string sensor_name;
    const int sensor_fd = openSensor(&sensor_name);
    if (params_fd < 0 || stats_fd < 0 || sensor_fd < 0) {
        std::cout << "Can not open " << params_path << ", " << stats_path << " or the sensor" << std::endl;
        return 1;
    }

    const SensorTuning &tuning = findTuning(sensor_name);
    std::cout << "Tuning " << (*tuning.name ? tuning.name : "imx290") << std::endl;

    // Sensor limits. Exposure in lines, kept within the frame so the frame rate stays, and at
    // least a line (auto exposure scales the current one, 0 would stay 0).
    v4l2_subdev_format fmt{};
    fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    xioctl(sensor_fd, VIDIOC_SUBDEV_G_FMT, &fmt);
    const int width = fmt.format.width, height = fmt.format.height;
    v4l2_queryctrl qexp, qgain, qvblank;
    hasControl(sensor_fd, V4L2_CID_EXPOSURE, &qexp);
    hasControl(sensor_fd, V4L2_CID_ANALOGUE_GAIN, &qgain);
    const int vblank = hasControl(sensor_fd, V4L2_CID_VBLANK, &qvblank) ? getControl(sensor_fd, V4L2_CID_VBLANK) : 0;
    const int min_exposure = std::max(1, qexp.minimum);
    const int max_exposure = std::min(qexp.maximum, height + vblank - 4);
    const int min_gain = qgain.minimum, max_gain = std::min(qgain.maximum, cfg.max_gain);
    std::cout << "Frame " << width << "x" << height << ", exposure " << min_exposure << ".." << max_exposure
              << " lines, gain " << min_gain << ".." << max_gain << std::endl;

    State state;
    // The driver's default can be longer than the frame (IMX219: 1575 lines, 881 at 60 fps).
    const int current_exposure = getControl(sensor_fd, V4L2_CID_EXPOSURE);
    state.exposure = std::clamp(current_exposure, min_exposure, max_exposure);
    if (state.exposure != current_exposure)
        setControl(sensor_fd, V4L2_CID_EXPOSURE, state.exposure);
    state.gain = getControl(sensor_fd, V4L2_CID_ANALOGUE_GAIN);
    if (cfg.fixed_wb_r > 0) {
        state.wb_r = cfg.fixed_wb_r;
        state.wb_b = cfg.fixed_wb_b;
    }
    if (cfg.fixed_exposure > 0) {
        state.exposure = std::clamp(cfg.fixed_exposure, min_exposure, max_exposure);
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
        fillParams(p, tuning, cfg, state, width, height, frame_id);
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
        // black level and the white balance gains. Auto exposure: all blocks but clipped ones.
        // White balance: gray world over the near neutral blocks only (after the current gains),
        // a scene full of grass, sky or a colored wall doesn't tint everything else; dark blocks
        // are mostly noise. All usable blocks if too few are neutral (e.g. the first estimate).
        double r = 0, g = 0, b = 0, wr = 0, wg = 0, wb = 0;
        int blocks = 0, neutral = 0, usable = 0;
        double ur = 0, ug = 0, ub = 0;
        for (const auto &d : stat->params.rawae3.data) {
            const double br = d.channelr_xy, bg = d.channelg_xy / 4.0, bb = d.channelb_xy;
            if (std::max({br, bg, bb}) > 1000.0)
                continue; // clipped, its color is unknown
            r += br;
            g += bg;
            b += bb;
            blocks++;
            if (bg < 16.0)
                continue;
            ur += br;
            ug += bg;
            ub += bb;
            usable++;
            if (std::abs(std::log(std::max(br, 1.0) / bg)) < AWB_NEUTRAL_LOG &&
                std::abs(std::log(std::max(bb, 1.0) / bg)) < AWB_NEUTRAL_LOG) {
                wr += br;
                wg += bg;
                wb += bb;
                neutral++;
            }
        }
        queueBuffer(stats_fd, V4L2_BUF_TYPE_META_CAPTURE, buf.index);
        if (neutral < usable / 10 || neutral < 8) {
            wr = ur;
            wg = ug;
            wb = ub;
        }

        if (blocks > 0) {
            r /= blocks;
            g /= blocks;
            b /= blocks;
            const double lg = std::max(1.0, g);

            // Gray world: the neutral average is gray. The statistics already include the gains,
            // so they are corrected by what is left, a third of the way per update.
            if (cfg.fixed_wb_r <= 0 && wg > 0) {
                const float cr = float(wg / std::max(wr, 1.0)), cb = float(wg / std::max(wb, 1.0));
                state.wb_r = std::clamp(state.wb_r * std::pow(cr, 0.3f), 0.5f, 4.0f);
                state.wb_b = std::clamp(state.wb_b * std::pow(cb, 0.3f), 0.5f, 4.0f);
            }
            state.ct = estimateCt(tuning, state.wb_r, state.wb_b);

            // Auto exposure on green, half way to the target per update (no oscillation with the
            // sensor's 1 to 2 frames delay). Exposure first, gain only above the longest exposure.
            if (cfg.fixed_exposure <= 0) {
                const double level = lg / (1023.0 - tuning.black_level_12bit / 4.0);
                const double ratio = std::clamp(std::sqrt(cfg.ae_target / std::max(level, 0.001)), 0.5, 2.0);
                const double total = std::clamp(double(state.exposure) * state.gain * ratio,
                                                double(min_exposure) * min_gain, double(max_exposure) * max_gain);
                const int exposure = std::clamp(int(total / min_gain), min_exposure, max_exposure);
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
                          << " blocks, " << neutral << " neutral) -> wb " << state.wb_r << " " << state.wb_b << " ("
                          << int(state.ct) << " K), exposure " << state.exposure << " gain " << state.gain << std::endl;
        }
        sendParams(frame_id);
    }
}
