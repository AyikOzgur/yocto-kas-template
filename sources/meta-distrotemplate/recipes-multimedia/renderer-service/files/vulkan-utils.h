#pragma once
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <tuple>
#include <unistd.h>
#include <vector>
#include <vulkan/vulkan.h>

void QueryInstanceExtensions();
VkInstance CreateVulkanInstance();
VkPhysicalDevice SelectPhysicalDevice(VkInstance instance, const std::string& device_path);
VkDevice CreateLogicalDevice(VkPhysicalDevice physical_device, int& queue_family_index);
void QueryDeviceExtensions(VkPhysicalDevice physical_device);
VkBuffer CreateVulkanBuffer(VkDevice device, VkDeviceMemory& memory,
                            int dma_buf_fd, int size_of_dma_buf, VkPhysicalDevice physical_device);
/// Host visible, coherent storage buffer, mapped persistently to *mapped.
VkBuffer CreateHostVisibleBuffer(VkDevice device, VkDeviceMemory& memory, VkDeviceSize size,
                                 VkPhysicalDevice physical_device, void** mapped);
/// Storage buffers at bindings 0 .. binding_count - 1.
VkDescriptorSetLayout CreateDescriptorSetLayout(VkDevice device, uint32_t binding_count);
VkPipelineLayout CreatePipelineLayout(VkDevice device, VkDescriptorSetLayout set_layout,
                                      uint32_t push_constant_size);
VkPipeline CreateComputePipeline(VkDevice device, VkPipelineLayout layout,
                                 const std::string& spirv_path);
VkDescriptorPool CreateDescriptorPool(VkDevice device, uint32_t max_sets,
                                      uint32_t buffers_per_set);
/// buffers[i] is bound to binding i.
VkDescriptorSet CreateDescriptorSet(VkDevice device, VkDescriptorPool pool,
                                    VkDescriptorSetLayout layout,
                                    const std::vector<VkBuffer>& buffers);
VkCommandPool CreateCommandPool(VkDevice device, int queue_family_index);
VkCommandBuffer CreateCommandBuffer(VkDevice device, VkCommandPool pool);
VkFence CreateFence(VkDevice device);