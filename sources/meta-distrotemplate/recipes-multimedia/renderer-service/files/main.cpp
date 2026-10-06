// Renderer test service.
//
// Draws inference-service detections on camera-service frames (YUYV) with
// the GPU and serves the result as dma-buf fds to streamer-service.
//
// Assumptions (test service, kept simple on purpose):
// - Latest detections are drawn on the latest camera frame, no frame matching
//   (camera-service frameId is not set yet). Boxes lag the video by roughly
//   the inference latency.
// - No buffer release protocol: an output buffer is reused when its camera
//   buffer comes back (4 x 40 ms ring), consumers must be done by then.

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

#include "Detections.h"
#include "FrameHeader.h"
#include "Renderer.h"
#include "SocketClient.h"
#include "SocketServer.h"

static const char *GPU_DEVICE = "/dev/dri/by-path/platform-fde60000.gpu-render";
static const char *CAMERA_SOCKET_PATH = "/run/camera-service.sock";
static const char *OUTPUT_SOCKET_PATH = "/run/renderer-service.sock";
static const int TIMEOUT_MS = 1000;
// Detections older than this are not drawn (inference stopped or stalled).
static const int MAX_DETECTION_AGE_MS = 500;

using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

int main(int argc, char **argv) {
    std::string gpu_device = GPU_DEVICE;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--gpu" && i + 1 < argc) {
            gpu_device = argv[++i];
        } else {
            std::cout << "Usage: " << argv[0] << " [--gpu " << GPU_DEVICE << "]" << std::endl;
            return 1;
        }
    }

    Renderer renderer(gpu_device);

    SocketClient camera;
    if (!camera.init(CAMERA_SOCKET_PATH)) {
        std::cout << CAMERA_SOCKET_PATH << " can not connect." << std::endl;
        return 1;
    }

    SocketServer output;
    if (!output.open(OUTPUT_SOCKET_PATH)) {
        std::cout << OUTPUT_SOCKET_PATH << " can not open." << std::endl;
        return 1;
    }

    DetectionSubscriber detections;
    DetectionsHeader det_hdr{};
    std::vector<DetectionBox> boxes;
    Clock::time_point det_time{};

    int frames = 0, drawn = 0;
    double render_ms = 0, max_ms = 0;
    auto window = Clock::now();

    while (true) {
        FrameHeader hdr{};
        int fd = -1;
        if (!camera.recvFrame(hdr, fd, TIMEOUT_MS)) {
            std::cout << "No camera frame in " << TIMEOUT_MS << " ms" << std::endl;
            continue;
        }

        if (detections.poll(DETECTIONS_SOCKET_PATH, det_hdr, boxes))
            det_time = Clock::now();

        auto t0 = Clock::now();

        RendererFrame background;
        background.dma_buf_fd = fd;
        background.width = hdr.width;
        background.height = hdr.height;
        background.stride = hdr.stride;
        background.fourcc = "YUYV";
        if (!renderer.updateBackroundImage(background)) {
            ::close(fd);
            continue;
        }

        if (det_hdr.modelWidth > 0 && ms_since(det_time) < MAX_DETECTION_AGE_MS) {
            // Undo preprocessing-service letterbox: model space -> camera frame.
            const int w = hdr.width, h = hdr.height;
            const float scale = std::min((float)det_hdr.modelWidth / w, (float)det_hdr.modelHeight / h);
            const int pad_x = (det_hdr.modelWidth - (int)(w * scale + 0.5f)) / 2;
            const int pad_y = (det_hdr.modelHeight - (int)(h * scale + 0.5f)) / 2;
            for (const auto &b : boxes) {
                int x0 = std::clamp((int)((b.x0 - pad_x) / scale), 0, w);
                int y0 = std::clamp((int)((b.y0 - pad_y) / scale), 0, h);
                int x1 = std::clamp((int)((b.x1 - pad_x) / scale), 0, w);
                int y1 = std::clamp((int)((b.y1 - pad_y) / scale), 0, h);
                if (renderer.drawRectangle(x1 - x0, y1 - y0, x0, y0))
                    drawn++;
            }
        }

        auto out = renderer.getRenderedFrame();
        if (out) {
            FrameHeader out_hdr{};
            out_hdr.frameId = hdr.frameId;
            out_hdr.width = out->width;
            out_hdr.height = out->height;
            out_hdr.stride = out->stride;
            out_hdr.length = out->stride * out->height;
            out_hdr.index = hdr.index;
            output.sendFrame(out_hdr, out->dma_buf_fd);
        }
        ::close(fd);

        double frame_ms = ms_since(t0);
        render_ms += frame_ms;
        max_ms = std::max(max_ms, frame_ms);
        frames++;

        double elapsed = ms_since(window);
        if (elapsed >= 1000.0) {
            std::cout << frames * 1000.0 / elapsed << " fps | render " << render_ms / frames
                      << " ms (max " << max_ms << ") | " << (double)drawn / frames
                      << " boxes/frame | detections age " << (int)ms_since(det_time) << " ms"
                      << std::endl;
            frames = drawn = 0;
            render_ms = max_ms = 0;
            window = Clock::now();
        }
    }
}
