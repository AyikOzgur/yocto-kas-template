
#include "vulkan-utils.h"


void QueryInstanceExtensions() {
  uint32_t instance_extension_count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &instance_extension_count,
                                         nullptr);

  std::vector<VkExtensionProperties> instance_extensions(
      instance_extension_count);
  vkEnumerateInstanceExtensionProperties(nullptr, &instance_extension_count,
                                         instance_extensions.data());

  bool supports_get_physical_device_properties_2 = false;
  for (const auto& extension : instance_extensions) {
    if (strcmp(extension.extensionName,
               VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0) {
      supports_get_physical_device_properties_2 = true;
      std::cout << "VK_KHR_get_physical_device_properties2 supported" << std::endl;
      break;
    }
  }

  if (!supports_get_physical_device_properties_2) {
    throw std::runtime_error(
        "VK_KHR_get_physical_device_properties2 is not supported");
  }
}


VkInstance CreateVulkanInstance() {
  VkApplicationInfo app_info = {};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pApplicationName = "Vulkan DMA-BUF Example";
  app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  app_info.pEngineName = "No Engine";
  app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
  // panvk on Mali-G52 is Vulkan 1.0 only, so use KHR extensions instead of 1.1 core.
  app_info.apiVersion = VK_API_VERSION_1_0;

  std::vector<const char*> instance_extensions = {
      VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME};

  VkInstanceCreateInfo instance_create_info = {};
  instance_create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instance_create_info.pApplicationInfo = &app_info;
  instance_create_info.enabledExtensionCount =
      static_cast<uint32_t>(instance_extensions.size());
  instance_create_info.ppEnabledExtensionNames = instance_extensions.data();

  VkInstance instance;
  if (vkCreateInstance(&instance_create_info, nullptr, &instance) !=
      VK_SUCCESS) {
    throw std::runtime_error("Failed to create Vulkan instance");
  }
  std::cout << "Vulkan instance created" << std::endl;

  return instance;
}

bool HasDeviceExtension(VkPhysicalDevice physical_device, const char* name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, extensions.data());
    for (const auto& extension : extensions) {
        if (strcmp(extension.extensionName, name) == 0)
            return true;
    }
    return false;
}

VkPhysicalDevice SelectPhysicalDevice(VkInstance instance, const std::string& device_path) {
    // Major/minor numbers of the given /dev/dri/cardX or /dev/dri/renderDX node.
    struct stat st;
    if (stat(device_path.c_str(), &st) < 0) {
        throw std::runtime_error("Can not stat " + device_path);
    }

    uint32_t device_count = 0;

    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);

    if (device_count == 0) {
        throw std::runtime_error("No Vulkan-compatible devices found");
    }

    std::vector<VkPhysicalDevice> devices(device_count);

    vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

    // KHR version, because core vkGetPhysicalDeviceProperties2 needs Vulkan 1.1.
    auto get_properties2 = (PFN_vkGetPhysicalDeviceProperties2KHR)
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2KHR");

    for (uint32_t i = 0; i < device_count; ++i) {
        // Devices without drm extension (like llvmpipe) can not match a /dev/dri node.
        if (!HasDeviceExtension(devices[i], VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME))
            continue;

        VkPhysicalDeviceDrmPropertiesEXT drm = {};
        drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 properties = {};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &drm;
        get_properties2(devices[i], &properties);

        bool is_primary = drm.hasPrimary && drm.primaryMajor == major(st.st_rdev) &&
                          drm.primaryMinor == minor(st.st_rdev);
        bool is_render = drm.hasRender && drm.renderMajor == major(st.st_rdev) &&
                         drm.renderMinor == minor(st.st_rdev);
        if (is_primary || is_render) {
            std::cout << "GPU " << i << ": " << properties.properties.deviceName
                      << " selected for " << device_path << std::endl;
            return devices[i];
        }
    }

    throw std::runtime_error("No vulkan device found for " + device_path);
}


VkDevice CreateLogicalDevice(VkPhysicalDevice physical_device,
                             int& queue_family_index) {
  uint32_t queue_family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count,
                                           nullptr);
  std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count,
                                           queue_families.data());

  for (size_t i = 0; i < queue_families.size(); i++) {
    if (queue_families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
      queue_family_index = i;
      break;
    }
  }

  if (queue_family_index == -1) {
    throw std::runtime_error("No suitable queue family found");
  }

  VkDeviceQueueCreateInfo queue_create_info = {};
  queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue_create_info.queueFamilyIndex = queue_family_index;
  queue_create_info.queueCount = 1;
  float queue_priority = 1.0f;
  queue_create_info.pQueuePriorities = &queue_priority;

  std::vector<const char*> device_extensions = {
      VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
      VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME};

  VkDeviceCreateInfo device_create_info = {};
  device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_create_info.pQueueCreateInfos = &queue_create_info;
  device_create_info.queueCreateInfoCount = 1;
  device_create_info.enabledExtensionCount =
      static_cast<uint32_t>(device_extensions.size());
  device_create_info.ppEnabledExtensionNames = device_extensions.data();

  VkDevice device;
  if (vkCreateDevice(physical_device, &device_create_info, nullptr, &device) !=
      VK_SUCCESS) {
    throw std::runtime_error("Failed to create Vulkan device");
  }

  return device;
}


void QueryDeviceExtensions(VkPhysicalDevice physical_device) {
  uint32_t device_extension_count = 0;
  vkEnumerateDeviceExtensionProperties(physical_device, nullptr,
                                       &device_extension_count, nullptr);

  std::vector<VkExtensionProperties> device_extensions(device_extension_count);
  vkEnumerateDeviceExtensionProperties(physical_device, nullptr,
                                       &device_extension_count,
                                       device_extensions.data());

  bool supports_external_memory = false;
  bool supports_external_memory_fd = false;
  bool supports_external_memory_dma_buf = false;

  for (const auto& extension : device_extensions) {
    if (strcmp(extension.extensionName,
               VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) == 0) {
      std::cout << "VK_KHR_external_memory supported" << std::endl;
      supports_external_memory = true;
    } else if (strcmp(extension.extensionName,
                      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0) {
      std::cout << "VK_KHR_external_memory_fd supported" << std::endl;
      supports_external_memory_fd = true;
    } else if (strcmp(extension.extensionName,
                      VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) == 0) {
      std::cout << "VK_EXT_external_memory_dma_buf supported" << std::endl;
      supports_external_memory_dma_buf = true;
    }
  }

  if (!supports_external_memory || !supports_external_memory_fd) {
    throw std::runtime_error(
        "Required external memory extensions are not supported");
  }
  if (!supports_external_memory_dma_buf) {
    throw std::runtime_error("VK_EXT_external_memory_dma_buf is not supported");
  }
}

uint32_t FindMemoryType(VkPhysicalDevice physical_device, uint32_t type_filter,
                        VkMemoryPropertyFlags properties) {
  VkPhysicalDeviceMemoryProperties mem_properties;
  vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_properties);

  for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
    if ((type_filter & (1 << i)) &&
        (mem_properties.memoryTypes[i].propertyFlags & properties) ==
            properties) {
      return i;
    }
  }
  throw std::runtime_error("Failed to find suitable memory type");
}

VkBuffer CreateVulkanBuffer(VkDevice device, VkDeviceMemory& memory,
                            int dma_buf_fd, int size_of_dma_buf, VkPhysicalDevice physical_device) {
  VkBufferCreateInfo buffer_create_info = {};
  buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_create_info.size = size_of_dma_buf;
  buffer_create_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkExternalMemoryBufferCreateInfo external_buffer_info = {};
  external_buffer_info.sType =
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
  external_buffer_info.handleTypes =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  buffer_create_info.pNext = &external_buffer_info;

  VkBuffer buffer;
  if (vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer) !=
      VK_SUCCESS) {
    throw std::runtime_error("Failed to create Vulkan buffer");
  }

  VkMemoryRequirements mem_requirements;
  vkGetBufferMemoryRequirements(device, buffer, &mem_requirements);

  VkImportMemoryFdInfoKHR import_memory_info = {};
  import_memory_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
  import_memory_info.handleType =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  import_memory_info.fd = dma_buf_fd;

  // Ask driver which memory types can hold this dma-buf.
  auto get_memory_fd_properties = (PFN_vkGetMemoryFdPropertiesKHR)
      vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR");
  VkMemoryFdPropertiesKHR fd_properties = {};
  fd_properties.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
  get_memory_fd_properties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                           dma_buf_fd, &fd_properties);

  VkMemoryAllocateInfo memory_allocate_info = {};
  memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memory_allocate_info.allocationSize = mem_requirements.size;
  memory_allocate_info.memoryTypeIndex =
      FindMemoryType(physical_device,
                     mem_requirements.memoryTypeBits & fd_properties.memoryTypeBits, 0);
  memory_allocate_info.pNext = &import_memory_info;

  // On success vulkan takes ownership of dma_buf_fd.
  VkResult result = vkAllocateMemory(device, &memory_allocate_info, nullptr, &memory);
  if (result != VK_SUCCESS) {
    throw std::runtime_error("vkAllocateMemory failed: " + std::to_string(result));
  }

  if (vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
    throw std::runtime_error("Failed to bind memory to Vulkan buffer");
  }

  return buffer;
}

VkBuffer CreateHostVisibleBuffer(VkDevice device, VkDeviceMemory& memory, VkDeviceSize size,
                                 VkPhysicalDevice physical_device, void** mapped) {
  VkBufferCreateInfo buffer_create_info = {};
  buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_create_info.size = size;
  buffer_create_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkBuffer buffer;
  if (vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer) != VK_SUCCESS) {
    throw std::runtime_error("Failed to create host visible buffer");
  }

  VkMemoryRequirements mem_requirements;
  vkGetBufferMemoryRequirements(device, buffer, &mem_requirements);

  VkMemoryAllocateInfo memory_allocate_info = {};
  memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memory_allocate_info.allocationSize = mem_requirements.size;
  memory_allocate_info.memoryTypeIndex =
      FindMemoryType(physical_device, mem_requirements.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  if (vkAllocateMemory(device, &memory_allocate_info, nullptr, &memory) != VK_SUCCESS) {
    throw std::runtime_error("Failed to allocate host visible memory");
  }
  if (vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
    throw std::runtime_error("Failed to bind host visible memory");
  }
  if (vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, mapped) != VK_SUCCESS) {
    throw std::runtime_error("Failed to map host visible memory");
  }
  return buffer;
}

VkDescriptorSetLayout CreateDescriptorSetLayout(VkDevice device, uint32_t binding_count) {
  std::vector<VkDescriptorSetLayoutBinding> bindings(binding_count);
  for (uint32_t i = 0; i < binding_count; i++) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }

  VkDescriptorSetLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layout_info.bindingCount = binding_count;
  layout_info.pBindings = bindings.data();

  VkDescriptorSetLayout layout;
  if (vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &layout) !=
      VK_SUCCESS) {
    throw std::runtime_error("Failed to create descriptor set layout");
  }
  return layout;
}

VkPipelineLayout CreatePipelineLayout(VkDevice device,
                                      VkDescriptorSetLayout set_layout,
                                      uint32_t push_constant_size) {
  VkPushConstantRange push_constant_range = {};
  push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_constant_range.offset = 0;
  push_constant_range.size = push_constant_size;

  VkPipelineLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_constant_range;

  VkPipelineLayout layout;
  if (vkCreatePipelineLayout(device, &layout_info, nullptr, &layout) !=
      VK_SUCCESS) {
    throw std::runtime_error("Failed to create pipeline layout");
  }
  return layout;
}

VkPipeline CreateComputePipeline(VkDevice device, VkPipelineLayout layout,
                                 const std::string& spirv_path) {
  std::ifstream file(spirv_path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open shader: " + spirv_path);
  }
  size_t file_size = file.tellg();
  std::vector<uint32_t> code(file_size / sizeof(uint32_t));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(code.data()), file_size);

  VkShaderModuleCreateInfo module_info = {};
  module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  module_info.codeSize = file_size;
  module_info.pCode = code.data();

  VkShaderModule shader_module;
  if (vkCreateShaderModule(device, &module_info, nullptr, &shader_module) !=
      VK_SUCCESS) {
    throw std::runtime_error("Failed to create shader module");
  }

  VkComputePipelineCreateInfo pipeline_info = {};
  pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline_info.stage.module = shader_module;
  pipeline_info.stage.pName = "main";
  pipeline_info.layout = layout;

  VkPipeline pipeline;
  if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                               nullptr, &pipeline) != VK_SUCCESS) {
    throw std::runtime_error("Failed to create compute pipeline");
  }

  // Module is not needed after pipeline is created.
  vkDestroyShaderModule(device, shader_module, nullptr);
  return pipeline;
}

VkDescriptorPool CreateDescriptorPool(VkDevice device, uint32_t max_sets,
                                      uint32_t buffers_per_set) {
  VkDescriptorPoolSize pool_size = {};
  pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  pool_size.descriptorCount = max_sets * buffers_per_set;

  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = max_sets;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;

  VkDescriptorPool pool;
  if (vkCreateDescriptorPool(device, &pool_info, nullptr, &pool) !=
      VK_SUCCESS) {
    throw std::runtime_error("Failed to create descriptor pool");
  }
  return pool;
}

VkDescriptorSet CreateDescriptorSet(VkDevice device, VkDescriptorPool pool,
                                    VkDescriptorSetLayout layout,
                                    const std::vector<VkBuffer>& buffers) {
  VkDescriptorSetAllocateInfo alloc_info = {};
  alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  alloc_info.descriptorPool = pool;
  alloc_info.descriptorSetCount = 1;
  alloc_info.pSetLayouts = &layout;

  VkDescriptorSet set;
  if (vkAllocateDescriptorSets(device, &alloc_info, &set) != VK_SUCCESS) {
    throw std::runtime_error("Failed to allocate descriptor set");
  }

  std::vector<VkDescriptorBufferInfo> buffer_infos(buffers.size());
  std::vector<VkWriteDescriptorSet> writes(buffers.size());
  for (uint32_t i = 0; i < buffers.size(); i++) {
    buffer_infos[i].buffer = buffers[i];
    buffer_infos[i].range = VK_WHOLE_SIZE;

    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &buffer_infos[i];
  }
  vkUpdateDescriptorSets(device, writes.size(), writes.data(), 0, nullptr);

  return set;
}

VkCommandPool CreateCommandPool(VkDevice device, int queue_family_index) {
  VkCommandPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = queue_family_index;

  VkCommandPool pool;
  if (vkCreateCommandPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS) {
    throw std::runtime_error("Failed to create command pool");
  }
  return pool;
}

VkCommandBuffer CreateCommandBuffer(VkDevice device, VkCommandPool pool) {
  VkCommandBufferAllocateInfo alloc_info = {};
  alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc_info.commandPool = pool;
  alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc_info.commandBufferCount = 1;

  VkCommandBuffer command_buffer;
  if (vkAllocateCommandBuffers(device, &alloc_info, &command_buffer) !=
      VK_SUCCESS) {
    throw std::runtime_error("Failed to allocate command buffer");
  }
  return command_buffer;
}

VkFence CreateFence(VkDevice device) {
  VkFenceCreateInfo fence_info = {};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

  VkFence fence;
  if (vkCreateFence(device, &fence_info, nullptr, &fence) != VK_SUCCESS) {
    throw std::runtime_error("Failed to create fence");
  }
  return fence;
}
