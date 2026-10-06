#pragma once
#include <memory>
#include <string>
#include <optional>

struct RendererFrame {
    int dma_buf_fd{-1};
    int width{-1};
    int height{-1};
    int stride{-1};
    std::string fourcc{"BGRA32"};
};

/// GPU (Vulkan compute) overlay renderer. Only "YUYV" frames are supported for now.
class Renderer {
public:
    /// @param device GPU DRM node, e.g. /dev/dri/by-path/platform-fde60000.gpu-render
    Renderer(const std::string& device);
    ~Renderer();
    Renderer(Renderer &rhs) = default;
    // Defaulted in Renderer.cpp, where RendererImpl is a complete type.
    Renderer(Renderer &&rhs);
    Renderer& operator=(Renderer &rhs) = default;
    Renderer& operator=(Renderer &&rhs);

    /// Set the frame to draw on. The renderer keeps its own reference to the
    /// dma-buf, the caller may close frame.dma_buf_fd afterwards.
    bool updateBackroundImage(const RendererFrame& frame);

    /// Queue a rectangle outline (pixels of the background frame) for the next getRenderedFrame().
    bool drawRectangle(int width, int height, int x, int y);

    /// Render background + queued rectangles into an output dma-buf owned by the
    /// renderer. The output buffer is reused when the same background buffer
    /// comes back, consumers must be done with it by then.
    std::optional<RendererFrame> getRenderedFrame();

private:
    class RendererImpl;
    std::unique_ptr<RendererImpl> m_impl;
};
