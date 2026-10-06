#include "Renderer.h"

#include <cstring>
#include <iostream>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "DmaHeap.h"
#include "vulkan-utils.h"

namespace {

/// Must match Params in overlay_yuyv.comp
struct PushConstants {
    int width;
    int height;
    int in_stride;
    int out_stride;
    int rect_count;
    int thickness;
};

const char *DMA_HEAP_DEVICE = "/dev/dma_heap/system";
const uint32_t MAX_RECTANGLES = 64;
const int LINE_THICKNESS = 2;
// Background sources use small buffer rings. If buffers keep changing (source
// restarted), drop everything instead of growing.
const uint32_t MAX_TARGETS = 8;

} // namespace

class Renderer::RendererImpl {
public:
    explicit RendererImpl(const std::string &device);
    ~RendererImpl();

    bool setBackground(const RendererFrame &frame);
    bool addRectangle(int width, int height, int x, int y);
    std::optional<RendererFrame> render();

private:
    /// Vulkan resources of one background dma-buf and its output buffer.
    struct Target {
        VkBuffer in_buffer{VK_NULL_HANDLE};
        VkDeviceMemory in_memory{VK_NULL_HANDLE};
        VkBuffer out_buffer{VK_NULL_HANDLE};
        VkDeviceMemory out_memory{VK_NULL_HANDLE};
        VkDescriptorSet set{VK_NULL_HANDLE};
        RendererFrame out;
    };

    bool m_ready{false};
    VkInstance m_instance{VK_NULL_HANDLE};
    VkPhysicalDevice m_physical_device{VK_NULL_HANDLE};
    VkDevice m_device{VK_NULL_HANDLE};
    VkQueue m_queue{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_set_layout{VK_NULL_HANDLE};
    VkPipelineLayout m_pipeline_layout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptor_pool{VK_NULL_HANDLE};
    VkCommandPool m_command_pool{VK_NULL_HANDLE};
    VkCommandBuffer m_command_buffer{VK_NULL_HANDLE};
    VkFence m_fence{VK_NULL_HANDLE};

    // MAX_RECTANGLES x (x0, y0, x1, y1), shared by all targets.
    VkBuffer m_rect_buffer{VK_NULL_HANDLE};
    VkDeviceMemory m_rect_memory{VK_NULL_HANDLE};
    int32_t *m_rects_mapped{nullptr};

    DmaHeap m_dma_heap;

    // Keyed by inode of the background dma-buf, a new fd arrives per frame.
    std::unordered_map<ino_t, Target> m_targets;
    Target *m_current{nullptr};
    RendererFrame m_background;
    std::vector<int32_t> m_rects;

    void destroyTarget(Target &target);
    void clearTargets();
};

Renderer::RendererImpl::RendererImpl(const std::string &device) {
    try {
        QueryInstanceExtensions();
        m_instance = CreateVulkanInstance();
        m_physical_device = SelectPhysicalDevice(m_instance, device);
        QueryDeviceExtensions(m_physical_device);
        int queue_family_index = -1;
        m_device = CreateLogicalDevice(m_physical_device, queue_family_index);
        vkGetDeviceQueue(m_device, queue_family_index, 0, &m_queue);

        // binding 0: background, 1: output, 2: rectangles.
        m_set_layout = CreateDescriptorSetLayout(m_device, 3);
        m_pipeline_layout = CreatePipelineLayout(m_device, m_set_layout, sizeof(PushConstants));
        m_pipeline = CreateComputePipeline(m_device, m_pipeline_layout, SHADER_PATH);
        m_descriptor_pool = CreateDescriptorPool(m_device, MAX_TARGETS, 3);
        m_command_pool = CreateCommandPool(m_device, queue_family_index);
        m_command_buffer = CreateCommandBuffer(m_device, m_command_pool);
        m_fence = CreateFence(m_device);

        void *mapped = nullptr;
        m_rect_buffer = CreateHostVisibleBuffer(m_device, m_rect_memory,
                                                MAX_RECTANGLES * 4 * sizeof(int32_t),
                                                m_physical_device, &mapped);
        m_rects_mapped = static_cast<int32_t *>(mapped);
    } catch (const std::exception &e) {
        std::cerr << "[renderer] Vulkan init failed: " << e.what() << std::endl;
        return;
    }

    if (!m_dma_heap.open(DMA_HEAP_DEVICE))
        return;

    m_ready = true;
}

Renderer::RendererImpl::~RendererImpl() {
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
        clearTargets();
        vkDestroyBuffer(m_device, m_rect_buffer, nullptr);
        vkFreeMemory(m_device, m_rect_memory, nullptr);
        vkDestroyFence(m_device, m_fence, nullptr);
        vkDestroyCommandPool(m_device, m_command_pool, nullptr);
        vkDestroyDescriptorPool(m_device, m_descriptor_pool, nullptr);
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        vkDestroyPipelineLayout(m_device, m_pipeline_layout, nullptr);
        vkDestroyDescriptorSetLayout(m_device, m_set_layout, nullptr);
        vkDestroyDevice(m_device, nullptr);
    }
    if (m_instance != VK_NULL_HANDLE)
        vkDestroyInstance(m_instance, nullptr);
}

void Renderer::RendererImpl::destroyTarget(Target &target) {
    vkDestroyBuffer(m_device, target.in_buffer, nullptr);
    vkFreeMemory(m_device, target.in_memory, nullptr);
    vkDestroyBuffer(m_device, target.out_buffer, nullptr);
    vkFreeMemory(m_device, target.out_memory, nullptr);
    if (target.out.dma_buf_fd >= 0)
        ::close(target.out.dma_buf_fd);
}

void Renderer::RendererImpl::clearTargets() {
    // GPU is idle here: every render() waits for its fence.
    for (auto &[key, target] : m_targets)
        destroyTarget(target);
    m_targets.clear();
    vkResetDescriptorPool(m_device, m_descriptor_pool, 0);
    m_current = nullptr;
}

bool Renderer::RendererImpl::setBackground(const RendererFrame &frame) {
    if (!m_ready)
        return false;
    if (frame.fourcc != "YUYV" || frame.dma_buf_fd < 0 || frame.width <= 0 || frame.height <= 0 ||
        frame.width % 2 != 0 || frame.stride < frame.width * 2 || frame.stride % 4 != 0) {
        std::cerr << "[renderer] unsupported background " << frame.width << "x" << frame.height
                  << " stride " << frame.stride << " " << frame.fourcc << std::endl;
        return false;
    }

    struct stat st;
    if (fstat(frame.dma_buf_fd, &st) < 0)
        return false;

    auto it = m_targets.find(st.st_ino);
    if (it == m_targets.end()) {
        if (m_targets.size() >= MAX_TARGETS)
            clearTargets();

        Target target;
        target.out.width = frame.width;
        target.out.height = frame.height;
        target.out.stride = frame.width * 2;
        target.out.fourcc = "YUYV";
        const int out_size = target.out.stride * frame.height;
        target.out.dma_buf_fd = m_dma_heap.allocate(out_size);
        if (target.out.dma_buf_fd < 0) {
            std::cerr << "[renderer] can not allocate output dma-buf" << std::endl;
            return false;
        }

        try {
            // Vulkan takes ownership of imported fds, so give it copies.
            target.in_buffer = CreateVulkanBuffer(m_device, target.in_memory, ::dup(frame.dma_buf_fd),
                                                  frame.stride * frame.height, m_physical_device);
            target.out_buffer = CreateVulkanBuffer(m_device, target.out_memory,
                                                   ::dup(target.out.dma_buf_fd), out_size,
                                                   m_physical_device);
            target.set = CreateDescriptorSet(m_device, m_descriptor_pool, m_set_layout,
                                             {target.in_buffer, target.out_buffer, m_rect_buffer});
        } catch (const std::exception &e) {
            std::cerr << "[renderer] can not import frame: " << e.what() << std::endl;
            destroyTarget(target);
            return false;
        }

        it = m_targets.emplace(st.st_ino, target).first;
        std::cout << "[renderer] new background buffer (inode " << st.st_ino << ")" << std::endl;
    }

    m_current = &it->second;
    m_background = frame;
    return true;
}

bool Renderer::RendererImpl::addRectangle(int width, int height, int x, int y) {
    if (width <= 0 || height <= 0 || m_rects.size() / 4 >= MAX_RECTANGLES)
        return false;
    m_rects.insert(m_rects.end(), {x, y, x + width, y + height});
    return true;
}

std::optional<RendererFrame> Renderer::RendererImpl::render() {
    if (!m_ready || !m_current)
        return std::nullopt;

    // Host coherent memory, visible to the GPU at vkQueueSubmit.
    std::memcpy(m_rects_mapped, m_rects.data(), m_rects.size() * sizeof(int32_t));

    PushConstants params{};
    params.width = m_background.width;
    params.height = m_background.height;
    params.in_stride = m_background.stride;
    params.out_stride = m_current->out.stride;
    params.rect_count = m_rects.size() / 4;
    params.thickness = LINE_THICKNESS;
    m_rects.clear();

    vkResetCommandBuffer(m_command_buffer, 0);
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_command_buffer, &begin_info);

    vkCmdBindPipeline(m_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(m_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout,
                            0, 1, &m_current->set, 0, nullptr);
    vkCmdPushConstants(m_command_buffer, m_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(PushConstants), &params);

    // One invocation per YUYV word (2 pixels), work group is 16x16.
    uint32_t group_x = (params.width / 2 + 15) / 16;
    uint32_t group_y = (params.height + 15) / 16;
    vkCmdDispatch(m_command_buffer, group_x, group_y, 1);

    vkEndCommandBuffer(m_command_buffer);

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &m_command_buffer;
    if (vkQueueSubmit(m_queue, 1, &submit_info, m_fence) != VK_SUCCESS) {
        std::cerr << "[renderer] vkQueueSubmit failed" << std::endl;
        return std::nullopt;
    }
    VkResult result = vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(m_device, 1, &m_fence);
    if (result != VK_SUCCESS) {
        std::cerr << "[renderer] vkWaitForFences failed: " << result << std::endl;
        return std::nullopt;
    }

    return m_current->out;
}

Renderer::Renderer(const std::string &device) : m_impl(std::make_unique<RendererImpl>(device)) {}

Renderer::~Renderer() = default;

Renderer::Renderer(Renderer &&rhs) = default;

Renderer &Renderer::operator=(Renderer &&rhs) = default;

bool Renderer::updateBackroundImage(const RendererFrame &frame) {
    return m_impl && m_impl->setBackground(frame);
}

bool Renderer::drawRectangle(int width, int height, int x, int y) {
    return m_impl && m_impl->addRectangle(width, height, x, y);
}

std::optional<RendererFrame> Renderer::getRenderedFrame() {
    if (!m_impl)
        return std::nullopt;
    return m_impl->render();
}
