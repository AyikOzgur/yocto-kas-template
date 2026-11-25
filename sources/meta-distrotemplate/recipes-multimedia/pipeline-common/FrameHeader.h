#pragma once
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <string>

/// @brief Fourcc code of a pixel format, the same values as V4L2_PIX_FMT_*.
constexpr uint32_t makeFourcc(char a, char b, char c, char d) {
    return uint32_t(a) | uint32_t(b) << 8 | uint32_t(c) << 16 | uint32_t(d) << 24;
}

/// @brief Pixel formats in the frame pipeline. Packed YUV 4:2:2 from the camera
/// (USB grabbers: YUYV, Rockchip ISP: UYVY), RGB 8:8:8 for inference.
constexpr uint32_t FOURCC_YUYV = makeFourcc('Y', 'U', 'Y', 'V');
constexpr uint32_t FOURCC_UYVY = makeFourcc('U', 'Y', 'V', 'Y');
constexpr uint32_t FOURCC_RGB24 = makeFourcc('R', 'G', 'B', '3');

/// @brief Printable fourcc, e.g. "YUYV".
inline std::string fourccName(uint32_t fourcc) {
    return {char(fourcc), char(fourcc >> 8), char(fourcc >> 16), char(fourcc >> 24)};
}

/// @brief Sent with every frame: consumers take the frame layout from here, not from
/// their own configuration.
struct __attribute__((packed)) FrameHeader {
    /// @brief Frame id.
    uint64_t frameId;
    /// @brief Frame width in pixels.
    uint32_t width;
    /// @brief Frame height in pixels.
    uint32_t height;
    /// @brief Frame stride in bytes.
    uint32_t stride;
    /// @brief Length of the frame data in bytes (e.g. for recvmsg).
    uint32_t length;
    /// @brief Frame index, usefull to track frames.
    uint8_t index;
    /// @brief Pixel format, one of the FOURCC_* codes.
    uint32_t fourcc;
};

static_assert(sizeof(FrameHeader) == 29, "FrameHeader must be 29 bytes");
