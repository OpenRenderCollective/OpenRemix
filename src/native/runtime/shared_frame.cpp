#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include "api/state.h"
#include "runtime/runtime.h"
#include <cstring>
#if defined(__linux__)
#include <unistd.h>
#endif

namespace remix {
#if defined(__linux__) || defined(_WIN32)
  namespace {
    // The editor shares the frame as opaque descriptors: file descriptors on
    // Linux, NT handles on Windows (the caller owns and closes them).
#if defined(_WIN32)
    constexpr auto kMemoryHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    constexpr auto kSemaphoreHandleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    void *&memoryHandle(remixapi_EditorSharedFrame &frame) { return frame.memory_handle; }
    void *&readyHandle(remixapi_EditorSharedFrame &frame) { return frame.ready_handle; }
    void *&releasedHandle(remixapi_EditorSharedFrame &frame) { return frame.released_handle; }
    constexpr void *kNoHandle = nullptr;
    void closeHandle(void *handle) {
      if (handle)
        CloseHandle(handle);
    }
#else
    constexpr auto kMemoryHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    constexpr auto kSemaphoreHandleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    int &memoryHandle(remixapi_EditorSharedFrame &frame) { return frame.memory_fd; }
    int &readyHandle(remixapi_EditorSharedFrame &frame) { return frame.ready_fd; }
    int &releasedHandle(remixapi_EditorSharedFrame &frame) { return frame.released_fd; }
    constexpr int kNoHandle = -1;
    void closeHandle(int fd) {
      if (fd >= 0)
        close(fd);
    }
#endif
  } // namespace
#endif

  int RemixNativeRuntime::configureSharedFrame(remixapi_EditorSharedFrame &frame) {
#if defined(__linux__) || defined(_WIN32)
    if (!m_initialized || !m_headless || m_sharedImage || frame.version != 1 || frame.width < 16 ||
        frame.height < 16 || frame.width > 8192 || frame.height > 8192)
      return -1;
    memoryHandle(frame) = readyHandle(frame) = releasedHandle(frame) = kNoHandle;
    auto check = [](VkResult result) {
      if (result != VK_SUCCESS)
        throw result;
    };
    try {
      const auto usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      VkPhysicalDeviceExternalImageFormatInfo externalFormat{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
      externalFormat.handleType = kMemoryHandleType;
      VkPhysicalDeviceImageFormatInfo2 format{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
      format.pNext = &externalFormat;
      format.format = VK_FORMAT_R8G8B8A8_UNORM;
      format.type = VK_IMAGE_TYPE_2D;
      format.tiling = VK_IMAGE_TILING_OPTIMAL;
      format.usage = usage;
      VkExternalImageFormatProperties externalProperties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
      VkImageFormatProperties2 properties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
      properties.pNext = &externalProperties;
      check(vkGetPhysicalDeviceImageFormatProperties2(m_physicalDevice, &format, &properties));
      if (!(externalProperties.externalMemoryProperties.externalMemoryFeatures &
            VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
        throw VK_ERROR_FORMAT_NOT_SUPPORTED;
      VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
      external.handleTypes = kMemoryHandleType;
      VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      image.pNext = &external;
      image.imageType = VK_IMAGE_TYPE_2D;
      image.format = VK_FORMAT_R8G8B8A8_UNORM;
      image.extent = {frame.width, frame.height, 1};
      image.mipLevels = image.arrayLayers = 1;
      image.samples = VK_SAMPLE_COUNT_1_BIT;
      image.tiling = VK_IMAGE_TILING_OPTIMAL;
      image.usage = usage;
      check(vkCreateImage(m_device, &image, nullptr, &m_sharedImage));
      VkMemoryRequirements requirements;
      vkGetImageMemoryRequirements(m_device, m_sharedImage, &requirements);
      VkPhysicalDeviceMemoryProperties memory;
      vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memory);
      uint32_t type = 0;
      for (; type < memory.memoryTypeCount; ++type)
        if ((requirements.memoryTypeBits & (1u << type)) &&
            (memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
          break;
      if (type == memory.memoryTypeCount)
        throw VK_ERROR_FEATURE_NOT_PRESENT;
      VkExportMemoryAllocateInfo exportMemory{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
      exportMemory.handleTypes = kMemoryHandleType;
      VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
      dedicated.image = m_sharedImage;
      dedicated.pNext = &exportMemory;
      VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      allocate.pNext = &dedicated;
      allocate.allocationSize = requirements.size;
      allocate.memoryTypeIndex = type;
      check(vkAllocateMemory(m_device, &allocate, nullptr, &m_sharedMemory));
      check(vkBindImageMemory(m_device, m_sharedImage, m_sharedMemory, 0));
      VkExportSemaphoreCreateInfo exportSemaphore{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
      exportSemaphore.handleTypes = kSemaphoreHandleType;
      VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      semaphore.pNext = &exportSemaphore;
      check(vkCreateSemaphore(m_device, &semaphore, nullptr, &m_sharedReady));
      check(vkCreateSemaphore(m_device, &semaphore, nullptr, &m_sharedReleased));
      m_swapchainImages = {m_sharedImage};
      m_swapchainFormat = VK_FORMAT_R8G8B8A8_UNORM;
      m_swapchainExtent = {frame.width, frame.height};
      VkFormatProperties imageProperties{};
      vkGetPhysicalDeviceFormatProperties(m_physicalDevice, m_swapchainFormat,
                                          &imageProperties);
      const auto blit =
          VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
      m_snapshotSupported =
          (imageProperties.optimalTilingFeatures & blit) == blit;
      if (createAttachments() != REMIXAPI_ERROR_CODE_SUCCESS)
        throw VK_ERROR_INITIALIZATION_FAILED;
#if defined(_WIN32)
      auto memoryExport = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
          vkGetDeviceProcAddr(m_device, "vkGetMemoryWin32HandleKHR"));
      auto semaphoreExport = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
          vkGetDeviceProcAddr(m_device, "vkGetSemaphoreWin32HandleKHR"));
      if (!memoryExport || !semaphoreExport)
        throw VK_ERROR_EXTENSION_NOT_PRESENT;
      VkMemoryGetWin32HandleInfoKHR getMemory{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
      getMemory.memory = m_sharedMemory;
      getMemory.handleType = kMemoryHandleType;
      check(memoryExport(m_device, &getMemory, &frame.memory_handle));
      VkSemaphoreGetWin32HandleInfoKHR getSemaphore{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
      getSemaphore.handleType = kSemaphoreHandleType;
      getSemaphore.semaphore = m_sharedReady;
      check(semaphoreExport(m_device, &getSemaphore, &frame.ready_handle));
      getSemaphore.semaphore = m_sharedReleased;
      check(semaphoreExport(m_device, &getSemaphore, &frame.released_handle));
#else
      auto memoryFd =
          reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(m_device, "vkGetMemoryFdKHR"));
      auto semaphoreFd =
          reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(m_device, "vkGetSemaphoreFdKHR"));
      if (!memoryFd || !semaphoreFd)
        throw VK_ERROR_EXTENSION_NOT_PRESENT;
      VkMemoryGetFdInfoKHR getMemory{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
      getMemory.memory = m_sharedMemory;
      getMemory.handleType = kMemoryHandleType;
      check(memoryFd(m_device, &getMemory, &frame.memory_fd));
      VkSemaphoreGetFdInfoKHR getSemaphore{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
      getSemaphore.handleType = kSemaphoreHandleType;
      getSemaphore.semaphore = m_sharedReady;
      check(semaphoreFd(m_device, &getSemaphore, &frame.ready_fd));
      getSemaphore.semaphore = m_sharedReleased;
      check(semaphoreFd(m_device, &getSemaphore, &frame.released_fd));
#endif
      VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
      VkPhysicalDeviceProperties2 device{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      device.pNext = &identity;
      vkGetPhysicalDeviceProperties2(m_physicalDevice, &device);
      std::memcpy(frame.device_uuid, identity.deviceUUID, 16);
      frame.memory_type = type;
      frame.allocation_size = requirements.size;
      return 0;
    } catch (...) {
      closeHandle(memoryHandle(frame));
      closeHandle(readyHandle(frame));
      closeHandle(releasedHandle(frame));
      memoryHandle(frame) = readyHandle(frame) = releasedHandle(frame) = kNoHandle;
      destroySwapchain();
      destroySharedFrame();
      return -2;
    }
#else
    return -3;
#endif
  }

  void RemixNativeRuntime::sharedBarrier(VkCommandBuffer command, bool acquire) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.image = m_sharedImage;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.oldLayout = acquire && !m_sharedPresented ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_EXTERNAL : m_queueFamily;
    barrier.dstQueueFamilyIndex = acquire ? m_queueFamily : VK_QUEUE_FAMILY_EXTERNAL;
    if (acquire && !m_sharedPresented)
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.srcAccessMask =
        acquire ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask =
        acquire ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : 0;
    vkCmdPipelineBarrier(command,
                         acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                 : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         acquire ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
                                 : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
  }

  void RemixNativeRuntime::destroySharedFrame() {
    if (m_sharedReady)
      vkDestroySemaphore(m_device, m_sharedReady, nullptr);
    if (m_sharedReleased)
      vkDestroySemaphore(m_device, m_sharedReleased, nullptr);
    if (m_sharedImage)
      vkDestroyImage(m_device, m_sharedImage, nullptr);
    if (m_sharedMemory)
      vkFreeMemory(m_device, m_sharedMemory, nullptr);
    m_sharedReady = m_sharedReleased = VK_NULL_HANDLE;
    m_sharedImage = VK_NULL_HANDLE;
    m_sharedMemory = VK_NULL_HANDLE;
    m_sharedPresented = false;
  }
} // namespace remix

extern "C" REMIXAPI int REMIXAPI_CALL remixapi_EditorConfigureSharedFrame(remixapi_EditorSharedFrame *frame) {
  if (!frame)
    return -1;
  std::lock_guard lock(remix::api::g_apiMutex);
  return remix::api::g_runtime.configureSharedFrame(*frame);
}
