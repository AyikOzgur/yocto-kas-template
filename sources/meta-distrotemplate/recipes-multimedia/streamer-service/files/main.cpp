// Streamer test service.
//
// Encodes renderer-service frames (YUYV dma-bufs) with the hardware JPEG
// encoder. Every JPEG is
// - sent as RTP/JPEG over UDP (--dest), play on the host with
//   tools/rtp-mjpeg.sdp in the yocto tree, and/or
// - appended to a raw MJPEG file in a directory (--record), play on the host with
//   ffplay -f mjpeg -framerate <fps> overlay-000.mjpeg

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "FrameHeader.h"
#include "RtpJpegSender.h"
#include "SocketClient.h"
#include "V4l2JpegEncoder.h"

static const char *INPUT_SOCKET_PATH = "/run/renderer-service.sock";
static const int TIMEOUT_MS = 1000;
static const int DEFAULT_QUALITY = 75;
static const int DEFAULT_RECORD_SECONDS = 60;

using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

/// Writes JPEG frames back to back (raw MJPEG) for a limited time into a new
/// numbered file in a directory. Existing recordings are never overwritten, the
/// file is only created when the first frame arrives (e.g. not on a boot
/// without camera).
class MjpegRecorder {
public:
    ~MjpegRecorder() { stop(); }

    void arm(const std::string &dir, int seconds) {
        m_dir = dir;
        m_limit = std::chrono::seconds(seconds);
        m_armed = true;
        std::cout << "[record] will record " << seconds << " s into " << dir << std::endl;
    }

    bool active() const { return m_armed; }

    void write(const std::vector<uint8_t> &jpeg) {
        if (!m_armed)
            return;
        if (!m_file && !open()) {
            m_armed = false;
            return;
        }
        const auto now = Clock::now();
        if (m_frames == 0)
            m_start = now;
        if (now - m_start >= m_limit) {
            stop();
            return;
        }
        // Flush every frame, so pulling the camera cable to plug in Ethernet loses nothing.
        if (std::fwrite(jpeg.data(), 1, jpeg.size(), m_file) != jpeg.size() || std::fflush(m_file) != 0) {
            std::cerr << "[record] write failed (disk full?): " << strerror(errno) << std::endl;
            stop();
            return;
        }
        m_frames++;
        m_bytes += jpeg.size();
    }

    void stop() {
        m_armed = false;
        if (!m_file)
            return;
        fsync(fileno(m_file));
        std::fclose(m_file);
        m_file = nullptr;
        const double seconds = m_frames ? std::chrono::duration<double>(Clock::now() - m_start).count() : 0;
        std::printf("[record] done: %zu frames, %.1f MB, %.1f fps average -> %s\n", m_frames,
                    m_bytes / (1024.0 * 1024.0), seconds > 0 ? m_frames / seconds : 0.0, m_path.c_str());
        std::fflush(stdout);
    }

private:
    std::string m_dir;
    std::string m_path;
    bool m_armed{false};
    std::FILE *m_file{nullptr};
    std::chrono::seconds m_limit{0};
    Clock::time_point m_start{};
    size_t m_frames{0};
    size_t m_bytes{0};

    bool open() {
        std::error_code ec;
        std::filesystem::create_directories(m_dir, ec);
        for (int i = 0; i < 1000 && !m_file; i++) {
            char name[32];
            std::snprintf(name, sizeof(name), "overlay-%03d.mjpeg", i);
            m_path = (std::filesystem::path(m_dir) / name).string();
            // "x": fail instead of overwriting an existing recording.
            m_file = std::fopen(m_path.c_str(), "wbx");
            if (!m_file && errno != EEXIST)
                break;
        }
        if (!m_file) {
            std::cerr << "[record] can not create a file in " << m_dir << ": " << strerror(errno) << std::endl;
            return false;
        }
        std::cout << "[record] recording to " << m_path << std::endl;
        return true;
    }
};

static void usage(const char *name) {
    std::cout << "Usage: " << name << " [--dest <ip>:<port>] [--record <dir> [--record-seconds "
              << DEFAULT_RECORD_SECONDS << "]] [--quality 5..100] [--device /dev/videoN]\n"
              << "At least one of --dest and --record is needed, an empty --dest disables RTP.\n"
              << "--record writes <dir>/overlay-NNN.mjpeg, a new file per recording." << std::endl;
}

int main(int argc, char **argv) {
    std::string dest, device, record_dir;
    int quality = DEFAULT_QUALITY;
    int record_seconds = DEFAULT_RECORD_SECONDS;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--dest" && i + 1 < argc) {
            dest = argv[++i];
        } else if (arg == "--record" && i + 1 < argc) {
            record_dir = argv[++i];
        } else if (arg == "--record-seconds" && i + 1 < argc) {
            record_seconds = std::atoi(argv[++i]);
        } else if (arg == "--quality" && i + 1 < argc) {
            quality = std::atoi(argv[++i]);
        } else if (arg == "--device" && i + 1 < argc) {
            device = argv[++i];
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if ((dest.empty() && record_dir.empty()) || quality < 5 || quality > 100 || record_seconds <= 0) {
        usage(argv[0]);
        return 1;
    }

    std::unique_ptr<RtpJpegSender> rtp;
    if (!dest.empty()) {
        const size_t colon = dest.rfind(':');
        const int port = colon == std::string::npos ? 0 : std::atoi(dest.c_str() + colon + 1);
        if (port <= 0 || port > 65535) {
            usage(argv[0]);
            return 1;
        }
        const std::string ip = dest.substr(0, colon);
        rtp = std::make_unique<RtpJpegSender>();
        if (!rtp->open(ip, port))
            return 1;
        std::cout << "Streaming RTP/JPEG (PT 26) to " << ip << ":" << port << std::endl;
    }

    MjpegRecorder recorder;
    if (!record_dir.empty())
        recorder.arm(record_dir, record_seconds);

    SocketClient input;
    if (!input.init(INPUT_SOCKET_PATH)) {
        std::cout << INPUT_SOCKET_PATH << " can not connect." << std::endl;
        return 1;
    }

    V4l2JpegEncoder encoder;

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

        // Nothing to do once recording is over and RTP is off, leave the encoder idle.
        if (!rtp && !recorder.active()) {
            ::close(fd);
            encoder.close();
            continue;
        }

        if (!encoder.isOpen() || encoder.width() != (int)hdr.width || encoder.height() != (int)hdr.height) {
            if (!encoder.open(device, hdr.width, hdr.height, quality)) {
                ::close(fd);
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
        }

        auto t0 = Clock::now();
        const auto &jpeg = encoder.encode(fd, hdr.length);
        ::close(fd);
        const double ms = ms_since(t0);
        if (jpeg.empty()) {
            encode_errors++;
            continue;
        }
        frames++;
        bytes += jpeg.size();
        encode_ms += ms;
        max_ms = std::max(max_ms, ms);

        recorder.write(jpeg);

        if (rtp) {
            // RTP clock is 90 kHz.
            const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now().time_since_epoch()).count();
            if (!rtp->send(jpeg.data(), jpeg.size(), (uint32_t)(now_us * 9 / 100)))
                send_errors++;
        }

        double elapsed = ms_since(window);
        if (elapsed >= 1000.0) {
            std::cout << frames * 1000.0 / elapsed << " fps | encode " << (frames ? encode_ms / frames : 0)
                      << " ms (max " << max_ms << ") | " << (frames ? bytes / frames / 1024 : 0)
                      << " KB/frame | " << bytes * 8 / elapsed / 1000.0 << " Mbit/s | errors encode "
                      << encode_errors << " send " << send_errors
                      << (recorder.active() ? " | recording" : "") << std::endl;
            frames = encode_errors = send_errors = 0;
            bytes = 0;
            encode_ms = max_ms = 0;
            window = Clock::now();
        }
    }
}
