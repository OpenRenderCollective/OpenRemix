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
    using Handle = void *;
    void *&memoryHandle(remixapi_EditorSharedFrame &frame) { return frame.memory_handle; }
    void *&readyHandle(remixapi_EditorSharedFrame &frame) { return frame.ready_handle; }
    void *&releasedHandle(remixapi_EditorSharedFrame &frame) { return frame.released_handle; }
    void *&depthHandle(remixapi_EditorSharedFrame2 &frame) { return frame.depth_memory_handle; }
    void *&uiHandle(remixapi_EditorSharedFrame3 &frame) { return frame.ui_memory_handle; }
    constexpr void *kNoHandle = nullptr;
    void closeHandle(void *handle) {
      if (handle)
        CloseHandle(handle);
    }
#else
    constexpr auto kMemoryHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    constexpr auto kSemaphoreHandleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    using Handle = int;
    int &memoryHandle(remixapi_EditorSharedFrame &frame) { return frame.memory_fd; }
    int &readyHandle(remixapi_EditorSharedFrame &frame) { return frame.ready_fd; }
    int &releasedHandle(remixapi_EditorSharedFrame &frame) { return frame.released_fd; }
    int &depthHandle(remixapi_EditorSharedFrame2 &frame) { return frame.depth_memory_fd; }
    int &uiHandle(remixapi_EditorSharedFrame3 &frame) { return frame.ui_memory_fd; }
    constexpr int kNoHandle = -1;
    void closeHandle(int fd) {
      if (fd >= 0)
        close(fd);
    }
#endif
    void check(VkResult result) {
      if (result != VK_SUCCESS)
        throw result;
    }

    uint32_t deviceLocalType(VkPhysicalDevice physical, uint32_t bits) {
      VkPhysicalDeviceMemoryProperties memory;
      vkGetPhysicalDeviceMemoryProperties(physical, &memory);
      for (uint32_t type = 0; type < memory.memoryTypeCount; ++type)
        if ((bits & (1u << type)) &&
            (memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
          return type;
      throw VK_ERROR_FEATURE_NOT_PRESENT;
    }

    // An image with dedicated memory another API can import.
    struct ExportedImage {
      VkImage image = VK_NULL_HANDLE;
      VkDeviceMemory memory = VK_NULL_HANDLE;
      uint32_t type = 0;
      VkDeviceSize size = 0;
    };

    ExportedImage createExported(VkPhysicalDevice physical, VkDevice device, VkFormat format,
                                 VkImageUsageFlags usage, VkExtent2D extent) {
      VkPhysicalDeviceExternalImageFormatInfo externalFormat{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
      externalFormat.handleType = kMemoryHandleType;
      VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
      info.pNext = &externalFormat;
      info.format = format;
      info.type = VK_IMAGE_TYPE_2D;
      info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage = usage;
      VkExternalImageFormatProperties externalProperties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
      VkImageFormatProperties2 properties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
      properties.pNext = &externalProperties;
      check(vkGetPhysicalDeviceImageFormatProperties2(physical, &info, &properties));
      if (!(externalProperties.externalMemoryProperties.externalMemoryFeatures &
            VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
        throw VK_ERROR_FORMAT_NOT_SUPPORTED;
      ExportedImage out;
      VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
      external.handleTypes = kMemoryHandleType;
      VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      image.pNext = &external;
      image.imageType = VK_IMAGE_TYPE_2D;
      image.format = format;
      image.extent = {extent.width, extent.height, 1};
      image.mipLevels = image.arrayLayers = 1;
      image.samples = VK_SAMPLE_COUNT_1_BIT;
      image.tiling = VK_IMAGE_TILING_OPTIMAL;
      image.usage = usage;
      check(vkCreateImage(device, &image, nullptr, &out.image));
      try {
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device, out.image, &requirements);
        out.type = deviceLocalType(physical, requirements.memoryTypeBits);
        out.size = requirements.size;
        VkExportMemoryAllocateInfo exportMemory{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        exportMemory.handleTypes = kMemoryHandleType;
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = out.image;
        dedicated.pNext = &exportMemory;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.pNext = &dedicated;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = out.type;
        check(vkAllocateMemory(device, &allocate, nullptr, &out.memory));
        check(vkBindImageMemory(device, out.image, out.memory, 0));
      } catch (...) {
        if (out.memory)
          vkFreeMemory(device, out.memory, nullptr);
        vkDestroyImage(device, out.image, nullptr);
        throw;
      }
      return out;
    }

    Handle exportMemory(VkDevice device, VkDeviceMemory memory) {
      Handle handle = kNoHandle;
#if defined(_WIN32)
      auto get = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
          vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR"));
      if (!get)
        throw VK_ERROR_EXTENSION_NOT_PRESENT;
      VkMemoryGetWin32HandleInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
      info.memory = memory;
      info.handleType = kMemoryHandleType;
      check(get(device, &info, &handle));
#else
      auto get = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR"));
      if (!get)
        throw VK_ERROR_EXTENSION_NOT_PRESENT;
      VkMemoryGetFdInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
      info.memory = memory;
      info.handleType = kMemoryHandleType;
      check(get(device, &info, &handle));
#endif
      return handle;
    }

    Handle exportSemaphore(VkDevice device, VkSemaphore semaphore) {
      Handle handle = kNoHandle;
#if defined(_WIN32)
      auto get = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
          vkGetDeviceProcAddr(device, "vkGetSemaphoreWin32HandleKHR"));
      if (!get)
        throw VK_ERROR_EXTENSION_NOT_PRESENT;
      VkSemaphoreGetWin32HandleInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
      info.semaphore = semaphore;
      info.handleType = kSemaphoreHandleType;
      check(get(device, &info, &handle));
#else
      auto get = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR"));
      if (!get)
        throw VK_ERROR_EXTENSION_NOT_PRESENT;
      VkSemaphoreGetFdInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
      info.semaphore = semaphore;
      info.handleType = kSemaphoreHandleType;
      check(get(device, &info, &handle));
#endif
      return handle;
    }
  } // namespace
#endif

  int RemixNativeRuntime::configureSharedFrame(remixapi_EditorSharedFrame &frame) {
#if defined(__linux__) || defined(_WIN32)
    // Versions 2 and 3 export depth and may be configured again; 3 the UI too.
    const bool second = frame.version == 2 || frame.version == 3;
    if (!m_initialized || !m_headless || (frame.version != 1 && !second) || (m_sharedImage && !second) ||
        frame.width < 16 || frame.height < 16 || frame.width > 8192 || frame.height > 8192)
      return -1;
    auto *withDepth = second ? reinterpret_cast<remixapi_EditorSharedFrame2 *>(&frame) : nullptr;
    auto *withUi = frame.version == 3 ? reinterpret_cast<remixapi_EditorSharedFrame3 *>(&frame) : nullptr;
    if (withUi) {
      uiHandle(*withUi) = kNoHandle;
      withUi->ui_allocation_size = 0;
      withUi->ui_memory_type = 0;
    }
    memoryHandle(frame) = readyHandle(frame) = releasedHandle(frame) = kNoHandle;
    if (withDepth) {
      depthHandle(*withDepth) = kNoHandle;
      withDepth->depth_allocation_size = 0;
      withDepth->depth_memory_type = 0;
    }
    if (m_sharedImage) {
      // A new size: the frames in flight, the attachments sized to the old
      // image and the image itself go; the tracer rebuilds at the new size.
      vkDeviceWaitIdle(m_device);
      destroySwapchain();
      destroySharedFrame();
    }
    try {
      const auto usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      const VkExtent2D extent{frame.width, frame.height};
      const ExportedImage colour =
          createExported(m_physicalDevice, m_device, VK_FORMAT_R8G8B8A8_UNORM, usage, extent);
      m_sharedImage = colour.image;
      m_sharedMemory = colour.memory;
      VkExportSemaphoreCreateInfo exportSemaphoreInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
      exportSemaphoreInfo.handleTypes = kSemaphoreHandleType;
      VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      semaphore.pNext = &exportSemaphoreInfo;
      check(vkCreateSemaphore(m_device, &semaphore, nullptr, &m_sharedReady));
      check(vkCreateSemaphore(m_device, &semaphore, nullptr, &m_sharedReleased));
      m_swapchainImages = {m_sharedImage};
      m_swapchainFormat = VK_FORMAT_R8G8B8A8_UNORM;
      m_swapchainExtent = extent;
      VkFormatProperties imageProperties{};
      vkGetPhysicalDeviceFormatProperties(m_physicalDevice, m_swapchainFormat,
                                          &imageProperties);
      const auto blit =
          VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
      m_snapshotSupported =
          (imageProperties.optimalTilingFeatures & blit) == blit;
      if (createAttachments() != REMIXAPI_ERROR_CODE_SUCCESS || (m_gui && !initializeUiVulkan()))
        throw VK_ERROR_INITIALIZATION_FAILED;
      // The depth attachment's texels go to the shared R32 image through a
      // buffer (a depth image copies to a colour one only through memory).
      if (withDepth && renderer().depthFormat() == VK_FORMAT_D32_SFLOAT) {
        const ExportedImage depth = createExported(
            m_physicalDevice, m_device, VK_FORMAT_R32_SFLOAT,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            extent);
        m_sharedDepthImage = depth.image;
        m_sharedDepthMemory = depth.memory;
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = VkDeviceSize(frame.width) * frame.height * 4;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        check(vkCreateBuffer(m_device, &buffer, nullptr, &m_sharedDepthBuffer));
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(m_device, m_sharedDepthBuffer, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = deviceLocalType(m_physicalDevice, requirements.memoryTypeBits);
        check(vkAllocateMemory(m_device, &allocate, nullptr, &m_sharedDepthBufferMemory));
        check(vkBindBufferMemory(m_device, m_sharedDepthBuffer, m_sharedDepthBufferMemory, 0));
        depthHandle(*withDepth) = exportMemory(m_device, m_sharedDepthMemory);
        withDepth->depth_allocation_size = depth.size;
        withDepth->depth_memory_type = depth.type;
      }
      // The developer UI's layer: drawn by a pass compatible with the
      // frame's (the UI's pipeline is made for that one), over the depth.
      if (withUi && m_gui) {
        const ExportedImage ui = createExported(
            m_physicalDevice, m_device, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            extent);
        m_sharedUiImage = ui.image;
        m_sharedUiMemory = ui.memory;
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = m_sharedUiImage;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_R8G8B8A8_UNORM;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(m_device, &view, nullptr, &m_sharedUiView));
        const VkAttachmentDescription attachments[] = {
            {.format = VK_FORMAT_R8G8B8A8_UNORM,
             .samples = VK_SAMPLE_COUNT_1_BIT,
             .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
             .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
             .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
             .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
             .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
             .finalLayout = VK_IMAGE_LAYOUT_GENERAL},
            {.format = renderer().depthFormat(),
             .samples = VK_SAMPLE_COUNT_1_BIT,
             .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
             .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
             .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
             .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
             .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
             .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
        const VkAttachmentReference colourRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        const VkAttachmentReference depthRef = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        const VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                              .colorAttachmentCount = 1,
                                              .pColorAttachments = &colourRef,
                                              .pDepthStencilAttachment = &depthRef};
        const VkSubpassDependency dependency = {
            .srcSubpass = VK_SUBPASS_EXTERNAL,
            .dstSubpass = 0,
            .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
        VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        pass.attachmentCount = 2;
        pass.pAttachments = attachments;
        pass.subpassCount = 1;
        pass.pSubpasses = &subpass;
        pass.dependencyCount = 1;
        pass.pDependencies = &dependency;
        check(vkCreateRenderPass(m_device, &pass, nullptr, &m_sharedUiPass));
        const VkImageView views[] = {m_sharedUiView, renderer().depthView()};
        VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = m_sharedUiPass;
        framebuffer.attachmentCount = 2;
        framebuffer.pAttachments = views;
        framebuffer.width = extent.width;
        framebuffer.height = extent.height;
        framebuffer.layers = 1;
        check(vkCreateFramebuffer(m_device, &framebuffer, nullptr, &m_sharedUiFramebuffer));
        uiHandle(*withUi) = exportMemory(m_device, m_sharedUiMemory);
        withUi->ui_allocation_size = ui.size;
        withUi->ui_memory_type = ui.type;
      }
      memoryHandle(frame) = exportMemory(m_device, m_sharedMemory);
      readyHandle(frame) = exportSemaphore(m_device, m_sharedReady);
      releasedHandle(frame) = exportSemaphore(m_device, m_sharedReleased);
      VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
      VkPhysicalDeviceProperties2 device{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      device.pNext = &identity;
      vkGetPhysicalDeviceProperties2(m_physicalDevice, &device);
      std::memcpy(frame.device_uuid, identity.deviceUUID, 16);
      frame.memory_type = colour.type;
      frame.allocation_size = colour.size;
      return 0;
    } catch (...) {
      closeHandle(memoryHandle(frame));
      closeHandle(readyHandle(frame));
      closeHandle(releasedHandle(frame));
      memoryHandle(frame) = readyHandle(frame) = releasedHandle(frame) = kNoHandle;
      if (withUi) {
        closeHandle(uiHandle(*withUi));
        uiHandle(*withUi) = kNoHandle;
        withUi->ui_allocation_size = 0;
      }
      if (withDepth) {
        closeHandle(depthHandle(*withDepth));
        depthHandle(*withDepth) = kNoHandle;
        withDepth->depth_allocation_size = 0;
      }
      destroySwapchain();
      destroySharedFrame();
      return -2;
    }
#else
    return -3;
#endif
  }

  void RemixNativeRuntime::sharedBarrier(VkCommandBuffer command, bool acquire) {
    const VkImage images[] = {m_sharedImage, m_sharedDepthImage, m_sharedUiImage};
    VkImageMemoryBarrier barriers[3];
    uint32_t count = 0;
    for (VkImage image : images) {
      if (!image)
        continue;
      VkImageMemoryBarrier &barrier = barriers[count++];
      barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.image = image;
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
    }
    vkCmdPipelineBarrier(command,
                         acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                 : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         acquire ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
                                 : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, count, barriers);
  }

  void RemixNativeRuntime::copySharedDepth(VkCommandBuffer command) {
    const VkImage depth = renderer().depthImage();
    if (!m_sharedDepthImage || !depth)
      return;
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    // The target's depth attachment, left in its attachment layout by the
    // pass that just ended, to the copy and back.
    VkImageMemoryBarrier toCopy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toCopy.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toCopy.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toCopy.srcQueueFamilyIndex = toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toCopy.image = depth;
    toCopy.subresourceRange = range;
    // The previous frame's copy out of the buffer has finished with it.
    VkBufferMemoryBarrier reuse{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    reuse.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    reuse.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    reuse.srcQueueFamilyIndex = reuse.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    reuse.buffer = m_sharedDepthBuffer;
    reuse.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &reuse, 1, &toCopy);
    const VkBufferImageCopy depthRegion = {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                                           .imageExtent = {m_swapchainExtent.width, m_swapchainExtent.height, 1}};
    vkCmdCopyImageToBuffer(command, depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_sharedDepthBuffer, 1,
                           &depthRegion);
    VkImageMemoryBarrier back = toCopy;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkBufferMemoryBarrier written = reuse;
    written.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    written.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         0, 0, nullptr, 1, &written, 1, &back);
    // The shared image stays GENERAL (sharedBarrier) for the consumer.
    const VkBufferImageCopy colourRegion = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                            .imageExtent = {m_swapchainExtent.width, m_swapchainExtent.height, 1}};
    vkCmdCopyBufferToImage(command, m_sharedDepthBuffer, m_sharedDepthImage, VK_IMAGE_LAYOUT_GENERAL, 1,
                           &colourRegion);
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
    if (m_sharedDepthImage)
      vkDestroyImage(m_device, m_sharedDepthImage, nullptr);
    if (m_sharedDepthMemory)
      vkFreeMemory(m_device, m_sharedDepthMemory, nullptr);
    if (m_sharedDepthBuffer)
      vkDestroyBuffer(m_device, m_sharedDepthBuffer, nullptr);
    if (m_sharedDepthBufferMemory)
      vkFreeMemory(m_device, m_sharedDepthBufferMemory, nullptr);
    if (m_sharedUiFramebuffer)
      vkDestroyFramebuffer(m_device, m_sharedUiFramebuffer, nullptr);
    if (m_sharedUiPass)
      vkDestroyRenderPass(m_device, m_sharedUiPass, nullptr);
    if (m_sharedUiView)
      vkDestroyImageView(m_device, m_sharedUiView, nullptr);
    if (m_sharedUiImage)
      vkDestroyImage(m_device, m_sharedUiImage, nullptr);
    if (m_sharedUiMemory)
      vkFreeMemory(m_device, m_sharedUiMemory, nullptr);
    m_sharedUiFramebuffer = VK_NULL_HANDLE;
    m_sharedUiPass = VK_NULL_HANDLE;
    m_sharedUiView = VK_NULL_HANDLE;
    m_sharedUiImage = VK_NULL_HANDLE;
    m_sharedUiMemory = VK_NULL_HANDLE;
    m_sharedReady = m_sharedReleased = VK_NULL_HANDLE;
    m_sharedImage = m_sharedDepthImage = VK_NULL_HANDLE;
    m_sharedMemory = m_sharedDepthMemory = m_sharedDepthBufferMemory = VK_NULL_HANDLE;
    m_sharedDepthBuffer = VK_NULL_HANDLE;
    m_sharedPresented = false;
  }
} // namespace remix

extern "C" REMIXAPI int REMIXAPI_CALL remixapi_EditorConfigureSharedFrame(remixapi_EditorSharedFrame *frame) {
  if (!frame)
    return -1;
  std::lock_guard lock(remix::api::g_apiMutex);
  return remix::api::g_runtime.configureSharedFrame(*frame);
}
