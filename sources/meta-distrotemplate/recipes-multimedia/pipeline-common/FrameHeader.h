#pragma once
#include <unistd.h>
#include <cstdint>
#include <cstring>

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
};

static_assert(sizeof(FrameHeader) == 25, "FrameHeader must be 25 bytes");
