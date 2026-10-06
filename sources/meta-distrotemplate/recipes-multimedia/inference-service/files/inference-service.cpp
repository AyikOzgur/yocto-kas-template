// NPU test consumer for preprocessing-service.
//
// Receives 640x640 RGB24 letterboxed frames as dma-buf fds over a unix
// socket, runs YOLO on the NPU with zero-copy input and prints FPS,
// timings and detections once per second. Detections of every frame are
// published on DETECTIONS_SOCKET_PATH (see Detections.h).
//
// Supported models (rknn_model_zoo exports, picked by output count):
// - YOLOv5: 3 outputs, anchor heads [1, 3*85, h, w]
// - YOLOv8: 9 outputs, per scale: box DFL [1, 64, h, w], class scores
//   [1, 80, h, w], score sum [1, 1, h, w]
//
// Assumptions (test service, kept simple on purpose):
// - Frames come from the linux,cma heap: the NPU runs without IOMMU and
//   only sees the first segment of a non-contiguous buffer.
// - No buffer release protocol. preprocessing-service reuses an output
//   buffer once per camera ring cycle (4 x 40 ms), so inference must
//   finish well within that. We always take the newest frame.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <rknn_api.h>

#include "Detections.h"
#include "FrameHeader.h"

static const char *SOCKET_PATH = "/run/preprocessing-service.sock";
static const char *MODEL_PATH = "/usr/share/inference-service/yolov8n.rknn";

// Minimum detection confidence. YOLO's usual 0.25 shows too many false detections
// on the analog camera; tune with --conf.
static const float DEFAULT_CONF_THRESH = 0.45f;
static const float NMS_THRESH = 0.45f;
static const int NUM_CLASSES = 80;
static const int PROP_SIZE = 5 + NUM_CLASSES;
static const int STRIDES[3] = {8, 16, 32};
static const int ANCHORS[3][6] = {
    {10, 13, 16, 30, 33, 23},
    {30, 61, 62, 45, 59, 119},
    {116, 90, 156, 198, 373, 326},
};

static const char *COCO[NUM_CLASSES] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat",
    "traffic light", "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat",
    "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe", "backpack",
    "umbrella", "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball",
    "kite", "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket",
    "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
    "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair",
    "couch", "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
    "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear", "hair drier",
    "toothbrush",
};

struct Detection {
    float x0, y0, x1, y1;
    float score;
    int cls;
};

using Clock = std::chrono::steady_clock;

static long npu_freq_mhz() {
    std::ifstream f("/sys/class/devfreq/fde40000.npu/cur_freq");
    long hz = 0;
    f >> hz;
    return hz / 1000000;
}

static double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

static int connect_socket(const char *path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Returns 1 on frame, 0 if nothing to read (non-blocking), -1 on disconnect.
static int recv_frame(int sock, FrameHeader &hdr, int &fd, int flags) {
    iovec iov{&hdr, sizeof(hdr)};
    union {
        cmsghdr cm;
        char buf[CMSG_SPACE(sizeof(int))];
    } ctrl{};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl.buf;
    msg.msg_controllen = sizeof(ctrl.buf);

    ssize_t n = ::recvmsg(sock, &msg, flags);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    if (n <= 0)
        return -1;

    cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS)
        return -1;
    std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
    if (static_cast<size_t>(n) < sizeof(hdr)) {
        ::close(fd);
        return -1;
    }
    return 1;
}

static float iou(const Detection &a, const Detection &b) {
    float w = std::max(0.f, std::min(a.x1, b.x1) - std::max(a.x0, b.x0));
    float h = std::max(0.f, std::min(a.y1, b.y1) - std::max(a.y0, b.y0));
    float inter = w * h;
    float uni = (a.x1 - a.x0) * (a.y1 - a.y0) + (b.x1 - b.x0) * (b.y1 - b.y0) - inter;
    return uni > 0.f ? inter / uni : 0.f;
}

static int8_t quantize(float f, const rknn_tensor_attr &attr) {
    return (int8_t)std::clamp(int(std::lround(f / attr.scale)) + attr.zp, -128, 127);
}

static float dequantize(int8_t q, const rknn_tensor_attr &attr) {
    return (float(q) - attr.zp) * attr.scale;
}

// Decode one YOLOv5 head: int8 NCHW [1, 3*85, gh, gw], sigmoid already applied in the model.
static void decode_v5_head(const int8_t *out, const rknn_tensor_attr &attr, int head,
                           int in_w, int in_h, float conf, std::vector<Detection> &dets) {
    const int stride = STRIDES[head];
    const int gw = in_w / stride, gh = in_h / stride, grid = gw * gh;
    auto deq = [&](int8_t q) { return dequantize(q, attr); };

    const int8_t thresh_q = quantize(conf, attr);

    for (int a = 0; a < 3; a++) {
        for (int y = 0; y < gh; y++) {
            for (int x = 0; x < gw; x++) {
                const int8_t *p = out + (PROP_SIZE * a) * grid + y * gw + x;
                if (p[4 * grid] < thresh_q)
                    continue;

                int best = 0;
                for (int k = 1; k < NUM_CLASSES; k++)
                    if (p[(5 + k) * grid] > p[(5 + best) * grid])
                        best = k;
                float score = deq(p[4 * grid]) * deq(p[(5 + best) * grid]);
                if (score < conf)
                    continue;

                float bx = (deq(p[0]) * 2.f - 0.5f + x) * stride;
                float by = (deq(p[grid]) * 2.f - 0.5f + y) * stride;
                float bw = deq(p[2 * grid]) * 2.f;
                float bh = deq(p[3 * grid]) * 2.f;
                bw = bw * bw * ANCHORS[head][a * 2];
                bh = bh * bh * ANCHORS[head][a * 2 + 1];
                dets.push_back({bx - bw / 2, by - bh / 2, bx + bw / 2, by + bh / 2, score, best});
            }
        }
    }
}

// Decode one YOLOv8 scale: box DFL [1, 4*dfl, gh, gw], class scores [1, 80, gh, gw]
// (sigmoid applied) and score sum [1, 1, gh, gw] used as a cheap pre-filter.
static void decode_v8_head(const int8_t *box, const rknn_tensor_attr &box_attr,
                           const int8_t *score, const rknn_tensor_attr &score_attr,
                           const int8_t *sum, const rknn_tensor_attr &sum_attr,
                           int in_h, float conf, std::vector<Detection> &dets) {
    const int gh = score_attr.dims[2], gw = score_attr.dims[3], grid = gh * gw;
    const int stride = in_h / gh;
    const int dfl = box_attr.dims[1] / 4;
    // The score sum is >= any class score, so it can only skip cells that fail anyway.
    const int8_t score_q = quantize(conf, score_attr);
    const int8_t sum_q = quantize(conf, sum_attr);

    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            const int off = y * gw + x;
            if (sum[off] < sum_q)
                continue;

            int best = -1;
            int8_t best_q = score_q;
            for (int c = 0; c < NUM_CLASSES; c++) {
                int8_t q = score[c * grid + off];
                if (q > best_q) {
                    best_q = q;
                    best = c;
                }
            }
            // The quantized compare is off by up to half a step, check the real value too.
            if (best < 0 || dequantize(best_q, score_attr) < conf)
                continue;

            // Distance to each side (l, t, r, b) is the expectation of a softmax over dfl bins.
            float d[4];
            for (int s = 0; s < 4; s++) {
                float e[16], e_sum = 0.f, acc = 0.f;
                for (int k = 0; k < dfl; k++) {
                    e[k] = std::exp(dequantize(box[(s * dfl + k) * grid + off], box_attr));
                    e_sum += e[k];
                }
                for (int k = 0; k < dfl; k++)
                    acc += e[k] / e_sum * k;
                d[s] = acc;
            }
            dets.push_back({(x + 0.5f - d[0]) * stride, (y + 0.5f - d[1]) * stride,
                            (x + 0.5f + d[2]) * stride, (y + 0.5f + d[3]) * stride,
                            dequantize(best_q, score_attr), best});
        }
    }
}

static std::vector<Detection> nms(std::vector<Detection> dets) {
    std::sort(dets.begin(), dets.end(), [](auto &a, auto &b) { return a.score > b.score; });
    std::vector<Detection> keep;
    for (auto &d : dets) {
        bool ok = true;
        for (auto &k : keep)
            if (k.cls == d.cls && iou(k, d) > NMS_THRESH) {
                ok = false;
                break;
            }
        if (ok)
            keep.push_back(d);
    }
    return keep;
}

int main(int argc, char **argv) {
    const char *model_path = MODEL_PATH;
    float conf = DEFAULT_CONF_THRESH;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--conf" && i + 1 < argc) {
            conf = std::atof(argv[++i]);
        } else if (arg[0] != '-') {
            model_path = argv[i];
        } else {
            conf = -1;
            break;
        }
    }
    if (conf <= 0.f || conf >= 1.f) {
        printf("Usage: %s [model.rknn] [--conf %.2f]\n", argv[0], DEFAULT_CONF_THRESH);
        return 1;
    }
    printf("confidence threshold %.2f\n", conf);

    std::ifstream file(model_path, std::ios::binary);
    std::vector<char> model((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (model.empty()) {
        printf("Can not read model %s\n", model_path);
        return 1;
    }

    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, model.data(), model.size(), 0, nullptr);
    if (ret < 0) {
        printf("rknn_init failed: %d\n", ret);
        return 1;
    }

    rknn_sdk_version ver{};
    rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver));
    printf("rknnrt %s, driver %s\n", ver.api_version, ver.drv_version);

    rknn_input_output_num io_num{};
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    const int n_out = io_num.n_output;
    const bool is_v8 = n_out == 9;
    if (io_num.n_input != 1 || (n_out != 3 && n_out != 9)) {
        printf("Expected YOLOv5 (3 outputs) or YOLOv8 (9 outputs), got %u inputs / %d outputs\n",
               io_num.n_input, n_out);
        return 1;
    }
    printf("model %s: %s\n", model_path, is_v8 ? "YOLOv8" : "YOLOv5");

    rknn_tensor_attr in_attr{};
    in_attr.index = 0;
    rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr));
    // NHWC: dims = [1, h, w, c]
    const int in_h = in_attr.dims[1], in_w = in_attr.dims[2], in_c = in_attr.dims[3];
    printf("input %dx%dx%d w_stride=%u size_with_stride=%u\n", in_w, in_h, in_c, in_attr.w_stride,
           in_attr.size_with_stride);
    if (in_attr.w_stride != 0 && (int)in_attr.w_stride != in_w) {
        printf("Model needs w_stride %u, preprocessing output is tightly packed\n", in_attr.w_stride);
        return 1;
    }
    // Feed raw uint8 RGB; the NPU applies mean/std and quantization.
    in_attr.type = RKNN_TENSOR_UINT8;
    in_attr.fmt = RKNN_TENSOR_NHWC;
    in_attr.pass_through = 0;

    std::vector<rknn_tensor_attr> out_attr(n_out);
    std::vector<rknn_tensor_mem *> out_mem(n_out);
    for (int i = 0; i < n_out; i++) {
        out_attr[i] = {};
        out_attr[i].index = i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &out_attr[i], sizeof(out_attr[i]));
        if (out_attr[i].type != RKNN_TENSOR_INT8) {
            printf("Output %d is not int8, only quantized models are supported\n", i);
            return 1;
        }
        // Keep native int8 NCHW output, decode_*_head dequantizes. Cacheable so CPU
        // reads are not one DRAM access per byte; rknn_mem_sync handles coherency.
        out_mem[i] = rknn_create_mem2(ctx, out_attr[i].size, RKNN_FLAG_MEMORY_CACHEABLE);
        if (!out_mem[i] || rknn_set_io_mem(ctx, out_mem[i], &out_attr[i]) < 0) {
            printf("Can not set output %d memory\n", i);
            return 1;
        }
    }
    // decode_v8_head keeps DFL bins on the stack.
    if (is_v8 && out_attr[0].dims[1] / 4 > 16) {
        printf("Unsupported YOLOv8 DFL length %u\n", out_attr[0].dims[1] / 4);
        return 1;
    }

    // Decode works on plain heap copies of the outputs: the NPU buffers may be
    // mapped uncached and decode reads them byte by byte with large strides.
    std::vector<std::vector<int8_t>> out_copy(n_out);
    for (int i = 0; i < n_out; i++)
        out_copy[i].resize(out_attr[i].size);

    // Imported input buffers, keyed by dma-buf inode (a new fd arrives per frame).
    std::unordered_map<ino_t, rknn_tensor_mem *> in_mems;

    DetectionPublisher publisher;
    if (!publisher.open(DETECTIONS_SOCKET_PATH)) {
        printf("Can not open %s\n", DETECTIONS_SOCKET_PATH);
        return 1;
    }
    std::vector<DetectionBox> boxes;

    int sock = -1;
    int frames = 0, dropped = 0;
    long candidates = 0;
    double run_ms = 0, sync_ms = 0, copy_ms = 0, decode_ms = 0, total_ms = 0, max_ms = 0;
    std::vector<Detection> last_dets;
    auto window = Clock::now();

    while (true) {
        if (sock < 0) {
            sock = connect_socket(SOCKET_PATH);
            if (sock < 0) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            printf("Connected to %s\n", SOCKET_PATH);
        }

        FrameHeader hdr{};
        int fd = -1;
        if (recv_frame(sock, hdr, fd, 0) < 0) {
            printf("Disconnected\n");
            ::close(sock);
            sock = -1;
            continue;
        }
        // Skip to the newest frame so we never work on a buffer about to be reused.
        FrameHeader next{};
        int next_fd = -1;
        while (recv_frame(sock, next, next_fd, MSG_DONTWAIT) == 1) {
            ::close(fd);
            hdr = next;
            fd = next_fd;
            dropped++;
        }

        auto t0 = Clock::now();

        struct stat st{};
        if (fstat(fd, &st) < 0 || (int)hdr.width != in_w || (int)hdr.height != in_h ||
            hdr.length < in_attr.size_with_stride) {
            printf("Unexpected frame %ux%u len %u\n", hdr.width, hdr.height, hdr.length);
            ::close(fd);
            continue;
        }

        auto it = in_mems.find(st.st_ino);
        if (it == in_mems.end()) {
            int own_fd = ::dup(fd);
            void *virt = mmap(nullptr, hdr.length, PROT_READ | PROT_WRITE, MAP_SHARED, own_fd, 0);
            rknn_tensor_mem *mem = virt == MAP_FAILED ? nullptr
                : rknn_create_mem_from_fd(ctx, own_fd, virt, in_attr.size_with_stride, 0);
            if (!mem) {
                printf("Can not import dma-buf (index %u)\n", hdr.index);
                if (virt != MAP_FAILED)
                    munmap(virt, hdr.length);
                ::close(own_fd);
                ::close(fd);
                continue;
            }
            it = in_mems.emplace(st.st_ino, mem).first;
            printf("Imported frame buffer index %u (inode %lu)\n", hdr.index, (unsigned long)st.st_ino);
        }
        ::close(fd);

        if ((ret = rknn_set_io_mem(ctx, it->second, &in_attr)) < 0) {
            printf("rknn_set_io_mem input failed: %d\n", ret);
            continue;
        }

        auto t1 = Clock::now();
        if ((ret = rknn_run(ctx, nullptr)) < 0) {
            printf("rknn_run failed: %d\n", ret);
            continue;
        }
        run_ms += ms_since(t1);

        auto t2 = Clock::now();
        for (int i = 0; i < n_out; i++)
            rknn_mem_sync(ctx, out_mem[i], RKNN_MEMORY_SYNC_FROM_DEVICE);
        sync_ms += ms_since(t2);

        auto t3 = Clock::now();
        for (int i = 0; i < n_out; i++)
            std::memcpy(out_copy[i].data(), out_mem[i]->virt_addr, out_attr[i].size);
        copy_ms += ms_since(t3);

        auto t4 = Clock::now();
        std::vector<Detection> dets;
        auto out = [&](int i) { return static_cast<const int8_t *>(out_copy[i].data()); };
        for (int h = 0; h < 3; h++) {
            if (is_v8)
                decode_v8_head(out(h * 3), out_attr[h * 3], out(h * 3 + 1), out_attr[h * 3 + 1],
                               out(h * 3 + 2), out_attr[h * 3 + 2], in_h, conf, dets);
            else
                decode_v5_head(out(h), out_attr[h], h, in_w, in_h, conf, dets);
        }
        candidates += dets.size();
        last_dets = nms(std::move(dets));
        decode_ms += ms_since(t4);

        boxes.clear();
        for (const auto &d : last_dets) {
            if (boxes.size() == MAX_DETECTIONS)
                break;
            boxes.push_back({d.x0, d.y0, d.x1, d.y1, d.score, d.cls});
        }
        DetectionsHeader det_hdr{};
        det_hdr.frameId = hdr.frameId;
        det_hdr.modelWidth = in_w;
        det_hdr.modelHeight = in_h;
        det_hdr.count = boxes.size();
        publisher.publish(det_hdr, boxes.data());

        double frame_ms = ms_since(t0);
        total_ms += frame_ms;
        max_ms = std::max(max_ms, frame_ms);
        frames++;

        double elapsed = ms_since(window);
        if (elapsed >= 1000.0) {
            printf("%.1f fps | npu %.1f ms @ %ld MHz | sync %.1f copy %.1f decode %.1f ms (%ld cand) | "
                   "total %.1f ms (max %.1f) | dropped %d | %zu objects:",
                   frames * 1000.0 / elapsed, run_ms / frames, npu_freq_mhz(), sync_ms / frames,
                   copy_ms / frames, decode_ms / frames, candidates / frames, total_ms / frames, max_ms,
                   dropped, last_dets.size());
            for (size_t i = 0; i < std::min<size_t>(last_dets.size(), 5); i++) {
                const auto &d = last_dets[i];
                printf(" %s %.2f [%.0f %.0f %.0f %.0f]", COCO[d.cls], d.score, d.x0, d.y0, d.x1, d.y1);
            }
            printf("\n");
            fflush(stdout);
            frames = dropped = 0;
            candidates = 0;
            run_ms = sync_ms = copy_ms = decode_ms = total_ms = max_ms = 0;
            window = Clock::now();
        }
    }
}
