#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace {
  void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
      throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
    }
  }

  void require(bool condition, const char* message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }

  VkDeviceSize align(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1) / alignment * alignment;
  }

  struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    void* pMapping = nullptr;
  };

  class Probe {
  public:
    ~Probe() {
      if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
        for (auto it = m_cleanup.rbegin(); it != m_cleanup.rend(); ++it) {
          (*it)();
        }
        vkDestroyDevice(m_device, nullptr);
      }
      if (m_instance != VK_NULL_HANDLE) {
        if (m_messenger != VK_NULL_HANDLE) {
          auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
          destroy(m_instance, m_messenger, nullptr);
        }
        vkDestroyInstance(m_instance, nullptr);
      }
    }

    void testBinarySemaphore() {
      VkSemaphore semaphore = VK_NULL_HANDLE;
      VkSemaphoreCreateInfo create = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      check(vkCreateSemaphore(m_device, &create, nullptr, &semaphore), "create binary semaphore");
      m_cleanup.push_back([this, semaphore] { vkDestroySemaphore(m_device, semaphore, nullptr); });
      VkSubmitInfo signal = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
      signal.signalSemaphoreCount = 1;
      signal.pSignalSemaphores = &semaphore;
      check(vkQueueSubmit(m_queue, 1, &signal, VK_NULL_HANDLE), "signal binary semaphore");
      const uint64_t ignoredValue = UINT64_MAX;
      VkTimelineSemaphoreSubmitInfo timeline = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
      timeline.waitSemaphoreValueCount = 1;
      timeline.pWaitSemaphoreValues = &ignoredValue;
      const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      VkSubmitInfo wait = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline};
      wait.waitSemaphoreCount = 1;
      wait.pWaitSemaphores = &semaphore;
      wait.pWaitDstStageMask = &stage;
      VkFence fence = VK_NULL_HANDLE;
      VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      check(vkCreateFence(m_device, &fenceInfo, nullptr, &fence), "create fence");
      m_cleanup.push_back([this, fence] { vkDestroyFence(m_device, fence, nullptr); });
      check(vkQueueSubmit(m_queue, 1, &wait, fence), "wait on binary semaphore");
      if (vkWaitForFences(m_device, 1, &fence, VK_TRUE, 5'000'000'000ull) != VK_SUCCESS) {
        std::fprintf(stderr, "FAIL: binary semaphore stalled with an ignored timeline value\n");
        // Device destruction would wait forever on the deliberately reproduced driver deadlock.
        std::_Exit(1);
      }
      require(m_validationErrors == 0, "Vulkan validation errors");
      std::puts("PASS: binary semaphore ignores timeline submit values");
    }

    void initialize(bool rayPipeline, bool bindless) {
      m_rayPipeline = rayPipeline;
      std::vector<const char*> extensions = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME
      };
      VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
      app.pApplicationName = "Remix native ray tracing probe";
      app.apiVersion = VK_API_VERSION_1_2;
      VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
      info.pApplicationInfo = &app;
      info.enabledExtensionCount = extensions.size();
      info.ppEnabledExtensionNames = extensions.data();
      VkDebugUtilsMessengerCreateInfoEXT debug = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
      debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
      debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
      debug.pfnUserCallback = reportError;
      debug.pUserData = this;
      info.pNext = &debug;
      check(vkCreateInstance(&info, nullptr, &m_instance), "vkCreateInstance");
      auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
      require(createMessenger != nullptr, "Missing debug messenger support");
      check(createMessenger(m_instance, &debug, nullptr, &m_messenger), "create validation messenger");
      uint32_t count = 0;
      check(vkEnumeratePhysicalDevices(m_instance, &count, nullptr), "enumerate GPUs");
      require(count != 0, "No Vulkan device");
      std::vector<VkPhysicalDevice> devices(count);
      check(vkEnumeratePhysicalDevices(m_instance, &count, devices.data()), "enumerate GPUs");
      m_physicalDevice = devices.front();

      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(m_physicalDevice, &properties);
      std::printf("Device: %s; API %u.%u.%u\n", properties.deviceName,
        VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion),
        VK_API_VERSION_PATCH(properties.apiVersion));

      check(vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &count, nullptr), "enumerate extensions");
      std::vector<VkExtensionProperties> available(count);
      check(vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &count, available.data()), "enumerate extensions");
      std::vector<const char*> required = {
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME
      };
      if (rayPipeline) {
        required.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
      }
      for (const char* name : required) {
        bool found = false;
        for (const auto& extension : available) {
          found |= std::strcmp(extension.extensionName, name) == 0;
        }
        if (!found) {
          throw std::runtime_error(std::string("Missing required extension: ") + name);
        }
      }

      VkPhysicalDeviceRayTracingPipelineFeaturesKHR tracing = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
      VkPhysicalDeviceRayQueryFeaturesKHR query = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, rayPipeline ? &tracing : nullptr};
      VkPhysicalDeviceAccelerationStructureFeaturesKHR accel = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &query};
      VkPhysicalDeviceVulkan12Features features12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &accel};
      VkPhysicalDeviceVulkan11Features features11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &features12};
      VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features11};
      vkGetPhysicalDeviceFeatures2(m_physicalDevice, &features);
      require(accel.accelerationStructure && query.rayQuery && (!rayPipeline || tracing.rayTracingPipeline),
        "Missing accelerationStructure, rayQuery, or rayTracingPipeline feature");
      require(features12.bufferDeviceAddress, "Missing buffer device address");
      require(!bindless || (features12.runtimeDescriptorArray &&
        features12.descriptorBindingPartiallyBound && features12.descriptorBindingStorageBufferUpdateAfterBind &&
        features12.descriptorBindingSampledImageUpdateAfterBind &&
        features12.shaderStorageBufferArrayNonUniformIndexing && features12.shaderSampledImageArrayNonUniformIndexing),
        "Missing Remix buffer-address or bindless descriptor features");

      VkPhysicalDeviceVulkan12Properties properties12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
      VkPhysicalDeviceAccelerationStructurePropertiesKHR accelProperties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR, &properties12};
      m_rtProperties.pNext = &accelProperties;
      VkPhysicalDeviceProperties2 properties2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, rayPipeline ? static_cast<void*>(&m_rtProperties) : &accelProperties};
      vkGetPhysicalDeviceProperties2(m_physicalDevice, &properties2);
      m_rtProperties.pNext = nullptr;
      m_scratchAlignment = accelProperties.minAccelerationStructureScratchOffsetAlignment;
      std::printf("Bindless per-stage limits: buffers=%u images=%u samplers=%u (Remix needs %u each)\n",
        properties12.maxPerStageDescriptorUpdateAfterBindStorageBuffers,
        properties12.maxPerStageDescriptorUpdateAfterBindSampledImages,
        properties12.maxPerStageDescriptorUpdateAfterBindSamplers, kTableSize);
      require(!bindless || (properties12.maxPerStageDescriptorUpdateAfterBindStorageBuffers >= kTableSize &&
        properties12.maxPerStageDescriptorUpdateAfterBindSampledImages >= kTableSize &&
        properties12.maxPerStageDescriptorUpdateAfterBindSamplers >= kTableSize),
        "Update-after-bind per-stage descriptor limits are below Remix table sizes");

      vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &count, nullptr);
      std::vector<VkQueueFamilyProperties> queues(count);
      vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &count, queues.data());
      uint32_t family = 0;
      while (family < count && !(queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        ++family;
      }
      require(family < count, "No compute queue");
      const float priority = 1.0f;
      VkDeviceQueueCreateInfo queueInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      queueInfo.queueFamilyIndex = family;
      queueInfo.queueCount = 1;
      queueInfo.pQueuePriorities = &priority;
      VkDeviceCreateInfo deviceInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
      deviceInfo.pNext = &features11;
      deviceInfo.queueCreateInfoCount = 1;
      deviceInfo.pQueueCreateInfos = &queueInfo;
      deviceInfo.enabledExtensionCount = required.size();
      deviceInfo.ppEnabledExtensionNames = required.data();
      VkPhysicalDeviceFeatures core = {};
      core.shaderInt16 = features.features.shaderInt16;
      core.shaderInt64 = features.features.shaderInt64;
      core.shaderStorageImageExtendedFormats = features.features.shaderStorageImageExtendedFormats;
      core.shaderStorageImageReadWithoutFormat = features.features.shaderStorageImageReadWithoutFormat;
      core.shaderStorageImageWriteWithoutFormat = features.features.shaderStorageImageWriteWithoutFormat;
      deviceInfo.pEnabledFeatures = &core;
      check(vkCreateDevice(m_physicalDevice, &deviceInfo, nullptr, &m_device), "vkCreateDevice");
      vkGetDeviceQueue(m_device, family, 0, &m_queue);

#define LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(m_device, #name)); require(name != nullptr, "Missing " #name)
      LOAD(vkGetBufferDeviceAddress);
      LOAD(vkCreateAccelerationStructureKHR);
      LOAD(vkDestroyAccelerationStructureKHR);
      LOAD(vkGetAccelerationStructureBuildSizesKHR);
      LOAD(vkCmdBuildAccelerationStructuresKHR);
      LOAD(vkGetAccelerationStructureDeviceAddressKHR);
      if (rayPipeline) {
        LOAD(vkCreateRayTracingPipelinesKHR);
        LOAD(vkGetRayTracingShaderGroupHandlesKHR);
        LOAD(vkCmdTraceRaysKHR);
      }
#undef LOAD

      VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      poolInfo.queueFamilyIndex = family;
      poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      check(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool), "create command pool");
      m_cleanup.push_back([this] { vkDestroyCommandPool(m_device, m_commandPool, nullptr); });
      VkCommandBufferAllocateInfo allocate = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      allocate.commandPool = m_commandPool;
      allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      allocate.commandBufferCount = 1;
      check(vkAllocateCommandBuffers(m_device, &allocate, &m_commands), "allocate commands");
    }

    void run(const std::filesystem::path& directory, const std::string& mode) {
      if (mode == "shader") {
        compileShader(directory);
        return;
      }
      constexpr std::array<float, 9> vertices = {-1, -1, 0, 1, -1, 0, 0, 1, 0};
      m_vertices = createBuffer(256, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
      std::memcpy(static_cast<char*>(m_vertices.pMapping) + kInputOffset, vertices.data(), sizeof(vertices));
      VkAccelerationStructureGeometryKHR geometry = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
      geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
      geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
      geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
      geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
      geometry.geometry.triangles.vertexData.deviceAddress = m_vertices.address + kInputOffset;
      geometry.geometry.triangles.vertexStride = 3 * sizeof(float);
      geometry.geometry.triangles.maxVertex = 2;
      geometry.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
      VkAccelerationStructureKHR blas = build(geometry, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);

      VkAccelerationStructureDeviceAddressInfoKHR addressInfo = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
      addressInfo.accelerationStructure = blas;
      VkAccelerationStructureInstanceKHR instance = {};
      instance.transform.matrix[0][0] = 1;
      instance.transform.matrix[1][1] = 1;
      instance.transform.matrix[2][2] = 1;
      instance.mask = 0xff;
      instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
      instance.accelerationStructureReference = vkGetAccelerationStructureDeviceAddressKHR(m_device, &addressInfo);
      m_instances = createBuffer(kInputOffset + sizeof(instance), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
      std::memcpy(static_cast<char*>(m_instances.pMapping) + kInputOffset, &instance, sizeof(instance));
      VkAccelerationStructureGeometryKHR instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
      instances.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
      instances.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
      instances.geometry.instances.data.deviceAddress = m_instances.address + kInputOffset;
      VkAccelerationStructureKHR tlas = build(instances, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
      m_results = createBuffer(2 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      setupDescriptors(tlas, mode == "bindless");
      const bool rayPipeline = mode == "pipeline";
      createPipeline(directory, rayPipeline, mode == "bindless");
      trace(rayPipeline, 1.0f);

      instance.transform.matrix[2][3] = 2.0f;
      std::memcpy(static_cast<char*>(m_instances.pMapping) + kInputOffset, &instance, sizeof(instance));
      build(instances, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, tlas);
      trace(rayPipeline, 3.0f);
      require(m_validationErrors == 0, "Vulkan validation reported errors");
      std::printf("PASS: %s, nonzero buffer offsets, hit/miss, TLAS update\n", mode.c_str());
    }

  private:
    bool m_rayPipeline = false;
    static constexpr uint32_t kTableSize = 65536;
    static constexpr VkDeviceSize kInputOffset = 128;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    std::atomic<uint32_t> m_validationErrors = 0;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_commands = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_sets;
    std::vector<std::function<void()>> m_cleanup;
    Buffer m_vertices, m_instances, m_results, m_sbt;
    VkDeviceSize m_sbtStride = 0;
    VkDeviceSize m_scratchAlignment = 256;
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_rtProperties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};

#define FUNCTION(name) PFN_##name name = nullptr
    FUNCTION(vkGetBufferDeviceAddress);
    FUNCTION(vkCreateAccelerationStructureKHR);
    FUNCTION(vkDestroyAccelerationStructureKHR);
    FUNCTION(vkGetAccelerationStructureBuildSizesKHR);
    FUNCTION(vkCmdBuildAccelerationStructuresKHR);
    FUNCTION(vkGetAccelerationStructureDeviceAddressKHR);
    FUNCTION(vkCreateRayTracingPipelinesKHR);
    FUNCTION(vkGetRayTracingShaderGroupHandlesKHR);
    FUNCTION(vkCmdTraceRaysKHR);
#undef FUNCTION

    static VKAPI_ATTR VkBool32 VKAPI_CALL reportError(VkDebugUtilsMessageSeverityFlagBitsEXT,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* pData, void* pUser) {
      auto* pProbe = static_cast<Probe*>(pUser);
      ++pProbe->m_validationErrors;
      std::fprintf(stderr, "Vulkan validation: %s\n", pData->pMessage);
      return VK_FALSE;
    }

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
      Buffer result;
      VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      info.size = size;
      info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
      check(vkCreateBuffer(m_device, &info, nullptr, &result.handle), "create buffer");
      VkMemoryRequirements requirements;
      vkGetBufferMemoryRequirements(m_device, result.handle, &requirements);
      VkPhysicalDeviceMemoryProperties properties;
      vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &properties);
      uint32_t type = 0;
      constexpr auto kFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      while (type < properties.memoryTypeCount &&
          (!(requirements.memoryTypeBits & (1u << type)) || (properties.memoryTypes[type].propertyFlags & kFlags) != kFlags)) {
        ++type;
      }
      require(type < properties.memoryTypeCount, "No coherent host-visible buffer memory");
      VkMemoryAllocateFlagsInfo flags = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
      flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
      VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
      const VkDeviceSize offset = align(256, requirements.alignment);
      allocate.allocationSize = requirements.size + offset;
      allocate.memoryTypeIndex = type;
      VkDeviceMemory memory = VK_NULL_HANDLE;
      check(vkAllocateMemory(m_device, &allocate, nullptr, &memory), "allocate memory");
      m_cleanup.push_back([this, memory, buffer = result.handle] {
        vkUnmapMemory(m_device, memory);
        vkDestroyBuffer(m_device, buffer, nullptr);
        vkFreeMemory(m_device, memory, nullptr);
      });
      check(vkBindBufferMemory(m_device, result.handle, memory, offset), "bind buffer memory");
      void* pMapping = nullptr;
      check(vkMapMemory(m_device, memory, 0, VK_WHOLE_SIZE, 0, &pMapping), "map buffer");
      result.pMapping = static_cast<char*>(pMapping) + offset;
      VkBufferDeviceAddressInfo addressInfo = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
      addressInfo.buffer = result.handle;
      result.address = vkGetBufferDeviceAddress(m_device, &addressInfo);
      require(result.address != 0, "Null buffer device address");
      return result;
    }

    void submit(const std::function<void()>& record) {
      check(vkResetCommandBuffer(m_commands, 0), "reset commands");
      VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      check(vkBeginCommandBuffer(m_commands, &begin), "begin commands");
      VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      vkCmdPipelineBarrier(m_commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
      record();
      barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier(m_commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
      check(vkEndCommandBuffer(m_commands), "end commands");
      VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submitInfo.commandBufferCount = 1;
      submitInfo.pCommandBuffers = &m_commands;
      check(vkQueueSubmit(m_queue, 1, &submitInfo, VK_NULL_HANDLE), "submit");
      check(vkQueueWaitIdle(m_queue), "wait for GPU");
    }

    VkAccelerationStructureKHR build(const VkAccelerationStructureGeometryKHR& geometry,
        VkAccelerationStructureTypeKHR type, VkAccelerationStructureKHR existing = VK_NULL_HANDLE) {
      VkAccelerationStructureBuildGeometryInfoKHR info = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
      info.type = type;
      info.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
      info.mode = existing ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
      info.geometryCount = 1;
      info.pGeometries = &geometry;
      uint32_t primitiveCount = 1;
      VkAccelerationStructureBuildSizesInfoKHR sizes = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
      vkGetAccelerationStructureBuildSizesKHR(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &primitiveCount, &sizes);
      VkAccelerationStructureKHR result = existing;
      if (!existing) {
        Buffer storage = createBuffer(sizes.accelerationStructureSize + 256, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
        VkAccelerationStructureCreateInfoKHR create = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        create.type = type;
        create.buffer = storage.handle;
        create.offset = 256;
        create.size = sizes.accelerationStructureSize;
        check(vkCreateAccelerationStructureKHR(m_device, &create, nullptr, &result), "create acceleration structure");
        m_cleanup.push_back([this, result] { vkDestroyAccelerationStructureKHR(m_device, result, nullptr); });
      }
      const VkDeviceSize scratchSize = existing ? sizes.updateScratchSize : sizes.buildScratchSize;
      Buffer scratch = createBuffer(scratchSize + m_scratchAlignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      info.srcAccelerationStructure = existing;
      info.dstAccelerationStructure = result;
      info.scratchData.deviceAddress = align(scratch.address, m_scratchAlignment);
      VkAccelerationStructureBuildRangeInfoKHR range = {};
      range.primitiveCount = 1;
      const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
      submit([&] { vkCmdBuildAccelerationStructuresKHR(m_commands, 1, &info, &pRange); });
      return result;
    }

    void setupDescriptors(VkAccelerationStructureKHR tlas, bool bindless) {
      const VkShaderStageFlags stages = VK_SHADER_STAGE_COMPUTE_BIT | (m_rayPipeline ? VK_SHADER_STAGE_RAYGEN_BIT_KHR : 0);
      std::array<VkDescriptorSetLayoutBinding, 2> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, stages, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr}
      }};
      VkDescriptorSetLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      layoutInfo.bindingCount = bindings.size();
      layoutInfo.pBindings = bindings.data();
      std::vector<VkDescriptorSetLayout> layouts(1);
      check(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &layouts[0]), "create descriptor layout");
      m_cleanup.push_back([this, layout = layouts[0]] { vkDestroyDescriptorSetLayout(m_device, layout, nullptr); });
      std::vector<VkDescriptorPoolSize> sizes = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
      if (bindless) {
        for (auto type : {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER}) {
          VkDescriptorSetLayoutBinding binding = {0, type, kTableSize, stages, nullptr};
          VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
          VkDescriptorSetLayoutBindingFlagsCreateInfo extended = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
          extended.bindingCount = 1;
          extended.pBindingFlags = &flags;
          layoutInfo.pNext = &extended;
          layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
          layoutInfo.bindingCount = 1;
          layoutInfo.pBindings = &binding;
          VkDescriptorSetLayout layout;
          check(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &layout), "create 65536-entry descriptor layout");
          layouts.push_back(layout);
          m_cleanup.push_back([this, layout] { vkDestroyDescriptorSetLayout(m_device, layout, nullptr); });
          sizes.push_back({type, kTableSize});
        }
      }
      VkDescriptorPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      poolInfo.flags = bindless ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0;
      poolInfo.maxSets = layouts.size();
      poolInfo.poolSizeCount = sizes.size();
      poolInfo.pPoolSizes = sizes.data();
      VkDescriptorPool pool;
      check(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &pool), "create descriptor pool");
      m_cleanup.push_back([this, pool] { vkDestroyDescriptorPool(m_device, pool, nullptr); });
      VkDescriptorSetAllocateInfo allocate = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      allocate.descriptorPool = pool;
      allocate.descriptorSetCount = layouts.size();
      allocate.pSetLayouts = layouts.data();
      m_sets.resize(layouts.size());
      check(vkAllocateDescriptorSets(m_device, &allocate, m_sets.data()), "allocate descriptors");
      VkWriteDescriptorSetAccelerationStructureKHR acceleration = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
      acceleration.accelerationStructureCount = 1;
      acceleration.pAccelerationStructures = &tlas;
      VkDescriptorBufferInfo bufferInfo = {m_results.handle, 0, VK_WHOLE_SIZE};
      std::array<VkWriteDescriptorSet, 2> writes = {};
      for (auto& write : writes) {
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_sets[0];
        write.descriptorCount = 1;
      }
      writes[0].pNext = &acceleration;
      writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
      writes[1].dstBinding = 1;
      writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[1].pBufferInfo = &bufferInfo;
      vkUpdateDescriptorSets(m_device, writes.size(), writes.data(), 0, nullptr);
      if (bindless) {
        Buffer input = createBuffer(sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        *static_cast<float*>(input.pMapping) = 1.0f;
        VkDescriptorBufferInfo inputInfo = {input.handle, 0, sizeof(float)};
        VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = m_sets[1];
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &inputInfo;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
        write.dstArrayElement = kTableSize - 1;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
      }
      VkPipelineLayoutCreateInfo pipelineLayoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      pipelineLayoutInfo.setLayoutCount = layouts.size();
      pipelineLayoutInfo.pSetLayouts = layouts.data();
      check(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_layout), "create pipeline layout");
      m_cleanup.push_back([this] { vkDestroyPipelineLayout(m_device, m_layout, nullptr); });
    }

    VkPipelineShaderStageCreateInfo shader(const std::filesystem::path& file, VkShaderStageFlagBits stage) {
      std::ifstream stream(file, std::ios::binary | std::ios::ate);
      require(stream.good(), "Could not open probe SPIR-V");
      const auto size = stream.tellg();
      require(size > 0 && size % 4 == 0, "Invalid SPIR-V size");
      std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
      stream.seekg(0);
      stream.read(reinterpret_cast<char*>(words.data()), size);
      require(stream.good(), "Could not read probe SPIR-V");
      VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      info.codeSize = size;
      info.pCode = words.data();
      VkShaderModule module;
      check(vkCreateShaderModule(m_device, &info, nullptr, &module), "create shader module");
      m_cleanup.push_back([this, module] { vkDestroyShaderModule(m_device, module, nullptr); });
      VkPipelineShaderStageCreateInfo result = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      result.stage = stage;
      result.module = module;
      result.pName = "main";
      return result;
    }

    void compileShader(const std::filesystem::path& file) {
      std::ifstream reflection(file.string() + ".bindings");
      require(reflection.good(), "Missing reflected shader bindings");
      std::array<std::vector<VkDescriptorSetLayoutBinding>, 4> sets;
      uint32_t set, binding, type, count;
      while (reflection >> set >> binding >> type >> count) {
        require(set < sets.size() && count > 0, "Invalid reflected shader binding");
        sets[set].push_back({binding, static_cast<VkDescriptorType>(type), count, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
      }
      require(reflection.eof(), "Malformed reflected shader binding");
      std::array<VkDescriptorSetLayout, 4> layouts;
      for (uint32_t index = 0; index < sets.size(); ++index) {
        std::vector<VkDescriptorBindingFlags> flags(sets[index].size(), index == 0 ? 0 :
          VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);
        VkDescriptorSetLayoutBindingFlagsCreateInfo extended = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        extended.bindingCount = flags.size();
        extended.pBindingFlags = flags.data();
        VkDescriptorSetLayoutCreateInfo info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &extended};
        info.flags = index == 0 ? 0 : VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        info.bindingCount = sets[index].size();
        info.pBindings = sets[index].data();
        check(vkCreateDescriptorSetLayout(m_device, &info, nullptr, &layouts[index]), "create reflected descriptor layout");
        m_cleanup.push_back([this, layout = layouts[index]] { vkDestroyDescriptorSetLayout(m_device, layout, nullptr); });
      }
      VkPipelineLayoutCreateInfo info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      info.setLayoutCount = layouts.size();
      info.pSetLayouts = layouts.data();
      check(vkCreatePipelineLayout(m_device, &info, nullptr, &m_layout), "create reflected pipeline layout");
      m_cleanup.push_back([this] { vkDestroyPipelineLayout(m_device, m_layout, nullptr); });
      VkComputePipelineCreateInfo pipelineInfo = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      pipelineInfo.stage = shader(file, VK_SHADER_STAGE_COMPUTE_BIT);
      pipelineInfo.layout = m_layout;
      std::printf("Compiling real Remix shader: %s\n", file.c_str());
      check(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline), "compile real Remix pipeline");
      m_cleanup.push_back([this] { vkDestroyPipeline(m_device, m_pipeline, nullptr); });
      require(m_validationErrors == 0, "Vulkan validation reported errors");
      std::printf("PASS: real Remix compute pipeline\n");
    }

    void createPipeline(const std::filesystem::path& directory, bool rayPipeline, bool bindless) {
      if (!rayPipeline) {
        VkComputePipelineCreateInfo info = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage = shader(directory / (bindless ? "probe_bindless.comp.spv" : "probe.comp.spv"), VK_SHADER_STAGE_COMPUTE_BIT);
        info.layout = m_layout;
        check(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &m_pipeline), "create ray-query pipeline");
      } else {
        std::array<VkPipelineShaderStageCreateInfo, 3> stages = {
          shader(directory / "probe.rgen.spv", VK_SHADER_STAGE_RAYGEN_BIT_KHR),
          shader(directory / "probe.rmiss.spv", VK_SHADER_STAGE_MISS_BIT_KHR),
          shader(directory / "probe.rchit.spv", VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR)
        };
        std::array<VkRayTracingShaderGroupCreateInfoKHR, 3> groups = {};
        for (uint32_t i = 0; i < groups.size(); ++i) {
          groups[i].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
          groups[i].type = i == 2 ? VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR : VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
          groups[i].generalShader = i == 2 ? VK_SHADER_UNUSED_KHR : i;
          groups[i].closestHitShader = i == 2 ? i : VK_SHADER_UNUSED_KHR;
          groups[i].anyHitShader = VK_SHADER_UNUSED_KHR;
          groups[i].intersectionShader = VK_SHADER_UNUSED_KHR;
        }
        VkRayTracingPipelineCreateInfoKHR info = {VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
        info.stageCount = stages.size();
        info.pStages = stages.data();
        info.groupCount = groups.size();
        info.pGroups = groups.data();
        info.maxPipelineRayRecursionDepth = 1;
        info.layout = m_layout;
        check(vkCreateRayTracingPipelinesKHR(m_device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &info, nullptr, &m_pipeline), "create ray-tracing pipeline");
        const uint32_t handleSize = m_rtProperties.shaderGroupHandleSize;
        m_sbtStride = align(handleSize, m_rtProperties.shaderGroupBaseAlignment);
        m_sbt = createBuffer(3 * m_sbtStride, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR);
        require(m_sbt.address % m_rtProperties.shaderGroupBaseAlignment == 0, "Unaligned SBT");
        std::vector<char> handles(3 * handleSize);
        check(vkGetRayTracingShaderGroupHandlesKHR(m_device, m_pipeline, 0, 3, handles.size(), handles.data()), "get shader group handles");
        for (uint32_t i = 0; i < 3; ++i) {
          std::memcpy(static_cast<char*>(m_sbt.pMapping) + i * m_sbtStride, handles.data() + i * handleSize, handleSize);
        }
      }
      m_cleanup.push_back([this] { vkDestroyPipeline(m_device, m_pipeline, nullptr); });
    }

    void trace(bool rayPipeline, float expected) {
      auto* pResults = static_cast<float*>(m_results.pMapping);
      pResults[0] = pResults[1] = -100.0f;
      submit([&] {
        auto point = rayPipeline ? VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR : VK_PIPELINE_BIND_POINT_COMPUTE;
        vkCmdBindPipeline(m_commands, point, m_pipeline);
        vkCmdBindDescriptorSets(m_commands, point, m_layout, 0, m_sets.size(), m_sets.data(), 0, nullptr);
        if (rayPipeline) {
          VkStridedDeviceAddressRegionKHR raygen = {m_sbt.address, m_sbtStride, m_sbtStride};
          VkStridedDeviceAddressRegionKHR miss = {m_sbt.address + m_sbtStride, m_sbtStride, m_sbtStride};
          VkStridedDeviceAddressRegionKHR hit = {m_sbt.address + 2 * m_sbtStride, m_sbtStride, m_sbtStride};
          VkStridedDeviceAddressRegionKHR callable = {};
          vkCmdTraceRaysKHR(m_commands, &raygen, &miss, &hit, &callable, 2, 1, 1);
        } else {
          vkCmdDispatch(m_commands, 2, 1, 1);
        }
      });
      std::printf("Readback: hit=%g (expected %g), miss=%g (expected -1)\n", pResults[0], expected, pResults[1]);
      require(std::isfinite(pResults[0]) && std::abs(pResults[0] - expected) < 0.001f && pResults[1] == -1.0f,
        "Incorrect ray tracing GPU readback");
    }
  };
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc != 3 || (std::strcmp(argv[2], "query") && std::strcmp(argv[2], "pipeline") && std::strcmp(argv[2], "bindless") && std::strcmp(argv[2], "shader") && std::strcmp(argv[2], "semaphore"))) {
    std::fprintf(stderr, "usage: %s SHADER_DIRECTORY query|pipeline|bindless, or SHADER_FILE shader\n", argv[0]);
    return 2;
  }
  try {
    Probe probe;
    probe.initialize(std::strcmp(argv[2], "pipeline") == 0,
      std::strcmp(argv[2], "bindless") == 0 || std::strcmp(argv[2], "shader") == 0);
    if (std::strcmp(argv[2], "semaphore") == 0) {
      probe.testBinarySemaphore();
    } else {
      probe.run(argv[1], argv[2]);
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
