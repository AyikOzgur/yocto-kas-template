// Streamer service.
//
// Encodes overlay-service frames (YUYV dma-bufs, imported without copies) to H.264 or
// H.265 on the Rockchip VEPU (MPP, vendor kernel) and serves them as MPEG-TS over SRT
// in listener mode. Receivers connect to the board, no destination to configure:
//   ffplay -fflags nobuffer -flags low_delay -framedrop "srt://<board-ip>:9000?latency=50000"
//   vlc --network-caching=50 --srt-latency=50 srt://<board-ip>:9000
// The encoder only runs while at least one receiver is connected, its bitrate follows
// the link between --min-bitrate and --bitrate (BitrateController).

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <unistd.h>

#include "BitrateController.h"
#include "FrameHeader.h"
#include "MppEncoder.h"
#include "SocketClient.h"
#include "SrtServer.h"
#include "TsMuxer.h"

static const char *INPUT_SOCKET_PATH = "/run/renderer-service.sock";
static const int TIMEOUT_MS = 1000;
static const int DEFAULT_PORT = 9000;
// Time SRT has for retransmissions, adds directly to glass-to-glass latency.
// Roughly 4x the round trip time is enough, raise it on lossy links.
static const int DEFAULT_LATENCY_MS = 50;
static const int DEFAULT_BITRATE_KBPS = 2000;
static const int DEFAULT_MIN_BITRATE_KBPS = 500;
static const int DEFAULT_FPS = 25;

using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

static void usage(const char *name) {
    std::cout << "Usage: " << name << " [--port " << DEFAULT_PORT << "] [--latency " << DEFAULT_LATENCY_MS
              << " ms] [--codec h264|h265]\n"
              << "       [--bitrate " << DEFAULT_BITRATE_KBPS << " kbps] [--min-bitrate " << DEFAULT_MIN_BITRATE_KBPS
              << " kbps] [--gop <frames>, default fps] [--fps " << DEFAULT_FPS << "]\n"
              << "The bitrate adapts to the link between --min-bitrate and --bitrate, equal values keep it fixed."
              << std::endl;
}

int main(int argc, char **argv) {
    std::string codec = "h264";
    int port = DEFAULT_PORT;
    int latency = DEFAULT_LATENCY_MS;
    int bitrate = DEFAULT_BITRATE_KBPS;
    int min_bitrate = DEFAULT_MIN_BITRATE_KBPS;
    int fps = DEFAULT_FPS;
    int gop = 0;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (arg == "--latency" && i + 1 < argc) {
            latency = std::atoi(argv[++i]);
        } else if (arg == "--codec" && i + 1 < argc) {
            codec = argv[++i];
        } else if (arg == "--bitrate" && i + 1 < argc) {
            bitrate = std::atoi(argv[++i]);
        } else if (arg == "--min-bitrate" && i + 1 < argc) {
            min_bitrate = std::atoi(argv[++i]);
        } else if (arg == "--gop" && i + 1 < argc) {
            gop = std::atoi(argv[++i]);
        } else if (arg == "--fps" && i + 1 < argc) {
            fps = std::atoi(argv[++i]);
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    // One IDR per second by default, new receivers get one right away anyway.
    if (gop == 0)
        gop = fps;
    const bool hevc = codec == "h265";
    if ((codec != "h264" && !hevc) || port <= 0 || port > 65535 || latency < 0 || bitrate <= 0 ||
        min_bitrate <= 0 || fps <= 0 || gop <= 0) {
        usage(argv[0]);
        return 1;
    }

    MppEncoder encoder(hevc, bitrate, gop, fps);
    TsMuxer muxer(hevc);
    SrtServer server;
    BitrateController bitrate_control(min_bitrate, bitrate, latency);
    if (!server.listen(port, latency))
        return 1;
    std::cout << "Serving " << codec << " MPEG-TS on srt://0.0.0.0:" << port << " (latency " << latency << " ms)"
              << std::endl;

    SocketClient input;
    if (!input.init(INPUT_SOCKET_PATH)) {
        std::cout << INPUT_SOCKET_PATH << " can not connect." << std::endl;
        return 1;
    }

    int frames = 0, encode_errors = 0, send_errors = 0;
    double encode_ms = 0, max_ms = 0;
    size_t bytes = 0;
    auto window = Clock::now();

    while (true) {
        FrameHeader hdr{};
        int fd = -1;
        if (!input.recvFrame(hdr, fd, TIMEOUT_MS)) {
            std::cout << "No frame in " << TIMEOUT_MS << " ms" << std::endl;
            continue;
        }

        // Start new receivers at a keyframe instead of waiting for the next GOP.
        if (server.acceptClients() > 0)
            encoder.requestKeyframe();

        // Nobody watching, leave the encoder idle.
        if (server.clients() == 0) {
            ::close(fd);
            encoder.close();
            continue;
        }

        if (!encoder.isOpen() || encoder.width() != (int)hdr.width || encoder.height() != (int)hdr.height) {
            if (!encoder.open(hdr.width, hdr.height)) {
                ::close(fd);
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
        }

        auto t0 = Clock::now();
        const auto &encoded = encoder.encode(fd, hdr.length);
        ::close(fd);
        const double ms = ms_since(t0);
        if (encoded.empty()) {
            encode_errors++;
            continue;
        }
        frames++;
        bytes += encoded.size();
        encode_ms += ms;
        max_ms = std::max(max_ms, ms);

        // 90 kHz clock for PTS / PCR.
        const auto now_us =
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
        const auto &ts = muxer.mux(encoded.data(), encoded.size(), now_us * 9 / 100);
        if (!server.send(ts.data(), ts.size()))
            send_errors++;

        double elapsed = ms_since(window);
        if (elapsed >= 1000.0) {
            const LinkStats link = server.stats();
            const int target = bitrate_control.update(link);
            if (target != encoder.bitrate())
                encoder.setBitrate(target);

            std::cout << frames * 1000.0 / elapsed << " fps | encode " << (frames ? encode_ms / frames : 0)
                      << " ms (max " << max_ms << ") | " << (frames ? bytes / frames / 1024 : 0)
                      << " KB/frame | " << bytes * 8 / elapsed / 1000.0 << " Mbit/s | clients "
                      << server.clients() << " | link lost " << link.lost << " retrans " << link.retransmitted
                      << "/" << link.sent << " dropped " << link.dropped << " buffer " << link.send_buffer_ms
                      << " ms rtt " << link.rtt_ms << " ms -> " << target << " kbps | errors encode "
                      << encode_errors << " send " << send_errors << std::endl;
            frames = encode_errors = send_errors = 0;
            bytes = 0;
            encode_ms = max_ms = 0;
            window = Clock::now();
        }
    }
}
