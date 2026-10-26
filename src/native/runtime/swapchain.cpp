#include "runtime/pipelines.h"
/*
 * Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "runtime/runtime.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace remix {
  namespace {
    bool succeeded(VkResult result) { return result == VK_SUCCESS; }
  } // namespace

  remixapi_ErrorCode RemixNativeRuntime::createSwapchain() {
    VkSurfaceCapabilitiesKHR capabilities = {};
    if (!succeeded(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &capabilities))) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    uint32_t formatCount = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, nullptr) !=
            VK_SUCCESS ||
        formatCount == 0) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, formats.data()) !=
            VK_SUCCESS ||
        !formatCount) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
    formats.resize(formatCount);

    uint32_t modeCount = 0;
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &modeCount, nullptr) !=
            VK_SUCCESS ||
        !modeCount) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
    std::vector<VkPresentModeKHR> modes(modeCount);
    if (modeCount != 0) {
      if (vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &modeCount, modes.data()) !=
          VK_SUCCESS) {
        return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      }
      modes.resize(modeCount);
    }

    remixapi_WindowState windowState = {};
    remixapi_ErrorCode status = m_window.getWindowState(windowState);
    if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
      return status;
    }

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == std::numeric_limits<uint32_t>::max()) {
      extent.width = std::clamp(windowState.drawableWidth, capabilities.minImageExtent.width,
                                capabilities.maxImageExtent.width);
      extent.height = std::clamp(windowState.drawableHeight, capabilities.minImageExtent.height,
                                 capabilities.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }

    uint32_t imageCount = std::max(2u, capabilities.minImageCount + 1);
    // Frame generation holds the images of a frame's generated frames and of
    // its rendered one until the GPU has finished it and they have been shown
    // in turn, while the next frame, already being recorded, acquires its own
    // and the frame before may still be showing its last (a surface with
    // fewer images generates for fewer frames: m_ungeneratedFrames).
    if (m_frameGenerationRequested)
      imageCount += 3 * m_frameGenerationRequested + 2;
    if (capabilities.maxImageCount != 0) {
      imageCount = std::min(imageCount, capabilities.maxImageCount);
    }
    if (imageCount < 2 || !(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    }
    const VkSurfaceFormatKHR format = chooseSurfaceFormat(formats);
    // With HDR10 the snapshot, host passes and screenshots take the float
    // back buffer; frame generation still copies the swapchain images.
    const bool hdr10 = format.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT;
    m_screenshotSupported =
        (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
        (format.format == VK_FORMAT_B8G8R8A8_SRGB || format.format == VK_FORMAT_B8G8R8A8_UNORM ||
         format.format == VK_FORMAT_R8G8B8A8_SRGB || format.format == VK_FORMAT_R8G8B8A8_UNORM || hdr10);
    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format.format,
                                        &formatProperties);
    const auto blit =
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    m_snapshotSupported =
        m_screenshotSupported &&
        (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
        (formatProperties.optimalTilingFeatures & blit) == blit;
    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((capabilities.supportedCompositeAlpha & compositeAlpha) == 0) {
      constexpr std::array<VkCompositeAlphaFlagBitsKHR, 3> kFallbackAlphaModes = {
          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
          VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
          VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
      };
      for (VkCompositeAlphaFlagBitsKHR mode : kFallbackAlphaModes) {
        if ((capabilities.supportedCompositeAlpha & mode) != 0) {
          compositeAlpha = mode;
          break;
        }
      }
    }

    // NVIDIA Reflex: the swapchain is created for it; the mode itself is
    // switched by applyLatencyMode.
    const VkSwapchainLatencyCreateInfoNV latency = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV,
                                                    .latencyModeEnable = VK_TRUE};
    const VkSwapchainCreateInfoKHR createInfo = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = m_lowLatencySupported ? &latency : nullptr,
        .surface = m_surface,
        .minImageCount = imageCount,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            (m_screenshotSupported ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0u) |
            (m_snapshotSupported ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0u),
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = compositeAlpha,
        .presentMode = (m_presentMode = choosePresentMode(modes)),
        .clipped = VK_TRUE,
    };
    if (!succeeded(vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &m_swapchain))) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    m_lowLatencyActive = false; // a new swapchain starts without it
    applyLatencyMode();
    m_presenterVsync = m_vsync;
    m_presenterFrameGeneration = m_frameGenerationRequested;
    m_frameGenerationFailed = false;
    m_frameGenerationStatus.clear();
    if (m_frameGenerationRequested || m_frameGenerationLog) {
      std::string available;
      for (auto mode : modes)
        available += " " + std::to_string(int(mode));
      logFrameGeneration("swapchain: %ux%u format %d | images asked %u (surface min %u max %u) | present mode %d "
                         "(0 immediate, 1 mailbox, 2 FIFO, 3 FIFO relaxed) of available%s | vsync %d | "
                         "frame generation asked %u | low latency supported %d | present metering supported %d",
                         extent.width, extent.height, int(format.format), imageCount, capabilities.minImageCount,
                         capabilities.maxImageCount, int(m_presentMode), available.c_str(), int(m_vsync),
                         m_frameGenerationRequested, int(m_lowLatencySupported), int(m_presentMeteringSupported));
    }
    m_swapchainFormat = format.format;
    m_swapchainExtent = extent;
    m_swapchainHdr10 = hdr10;
    if (vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, nullptr) != VK_SUCCESS ||
        imageCount < 2) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
    m_swapchainImages.resize(imageCount);
    if (!succeeded(vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, m_swapchainImages.data()))) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
    m_swapchainImages.resize(imageCount);
    if (m_frameGenerationRequested || m_frameGenerationLog)
      logFrameGeneration("swapchain: %u images created", imageCount);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode RemixNativeRuntime::recreateSwapchain() {
    finishDeferredPresent(); // the presenter still shows images of this swapchain
    VkResult result = vkDeviceWaitIdle(m_device);
    if (result != VK_SUCCESS) {
      return fail(result);
    }
    destroySwapchain();
    if (m_recreateSurface) {
      if (m_surface) {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
      }
      m_surface = VK_NULL_HANDLE;
      auto status = m_window.createVulkanSurface(m_instance, nullptr, m_surface);
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        return status;
      }
      VkBool32 supported = VK_FALSE;
      result = vkGetPhysicalDeviceSurfaceSupportKHR(m_physicalDevice, m_queueFamily, m_surface, &supported);
      if (result != VK_SUCCESS || !supported) {
        return fail(result);
      }
      m_recreateSurface = false;
    }
    m_rebuild = false;
    auto status = createSwapchain();
    if (status != REMIXAPI_ERROR_CODE_SUCCESS || !m_swapchain) {
      return status;
    }
    status = createAttachments();
    if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
      return status;
    }
    return initializeUiVulkan() ? REMIXAPI_ERROR_CODE_SUCCESS : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }

  remixapi_ErrorCode RemixNativeRuntime::createAttachments() {
    m_snapshotAllocationFailed = false;
    // HDR10: the frame's passes draw into a float back buffer (GENERAL, as
    // the shared image of headless mode), which HdrOutput encodes into the
    // swapchain image.
    const bool backBuffer = m_headless || m_swapchainHdr10;
    VkAttachmentDescription attachments[] = {
        {
            .format = m_swapchainHdr10 ? HdrOutput::backBufferFormat : m_swapchainFormat,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = backBuffer ? VK_IMAGE_LAYOUT_GENERAL
                                        : VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = backBuffer ? VK_IMAGE_LAYOUT_GENERAL
                                      : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        },
        {
            .format = renderer().depthFormat(),
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            // Stored: a frame with halo outlines leaves the pass after the
            // scene and depth-tests their silhouettes against it.
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        }};
    const VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depth = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
        .pDepthStencilAttachment = &depth,
    };
    const VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_TRANSFER_READ_BIT |
                         VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    };
    const VkRenderPassCreateInfo pass = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2,
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    if (pipeline::createRenderPass(m_device, &pass, nullptr, &m_renderPass) !=
        VK_SUCCESS) {
      return fail();
    }
    // Compatible with the scene pass: preserve its color while drawing fresh
    // controls after capture, or over the retained image during preparation.
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].initialLayout = attachments[0].finalLayout;
    if (pipeline::createRenderPass(m_device, &pass, nullptr, &m_overlayPass) !=
        VK_SUCCESS)
      return fail();
    // Re-entered within a frame, right after the scene pass (halo outlines):
    // preserves the scene's depth as well.
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].initialLayout = attachments[1].finalLayout;
    if (pipeline::createRenderPass(m_device, &pass, nullptr, &m_resumePass) != VK_SUCCESS)
      return fail();
    // The UNORM swapchain (or HDR10's float back buffer) takes display values:
    // the draws encode gamma 2.2.
    renderer().configureTarget(m_renderPass, m_swapchainExtent, true);
    const uint32_t count = static_cast<uint32_t>(m_swapchainImages.size());
    m_imageViews.resize(count);
    m_framebuffers.resize(count);
    m_renderFinished.resize(count);
    m_commandBuffers.resize(gpuFrameCount);
    const VkCommandBufferAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = m_commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = gpuFrameCount,
    };
    if (vkAllocateCommandBuffers(m_device, &allocate, m_commandBuffers.data()) != VK_SUCCESS) {
      m_commandBuffers.clear();
      return fail();
    }
    for (uint32_t i = 0; i < count; ++i) {
      const VkImageViewCreateInfo view = {
          .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
          .image = m_swapchainImages[i],
          .viewType = VK_IMAGE_VIEW_TYPE_2D,
          .format = m_swapchainFormat,
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      if (vkCreateImageView(m_device, &view, nullptr, &m_imageViews[i]) != VK_SUCCESS) {
        return fail();
      }
      const VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      if (vkCreateSemaphore(m_device, &semaphore, nullptr, &m_renderFinished[i]) != VK_SUCCESS) {
        return fail();
      }
      // HDR10's passes draw into its back buffers' framebuffers instead.
      if (m_swapchainHdr10)
        continue;
      const VkImageView views[] = {m_imageViews[i], renderer().depthView()};
      const VkFramebufferCreateInfo framebuffer = {
          .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
          .renderPass = m_renderPass,
          .attachmentCount = 2,
          .pAttachments = views,
          .width = m_swapchainExtent.width,
          .height = m_swapchainExtent.height,
          .layers = 1,
      };
      if (vkCreateFramebuffer(m_device, &framebuffer, nullptr, &m_framebuffers[i]) != VK_SUCCESS) {
        return fail();
      }
    }
    if (m_swapchainHdr10) {
      try {
        m_hdrOutput.create(m_physicalDevice, m_device, m_swapchainFormat, m_swapchainExtent, m_imageViews,
                           gpuFrameCount, m_renderPass, renderer().depthView());
      } catch (VkResult result) {
        return fail(result);
      }
    }
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  void RemixNativeRuntime::destroySwapchain() {
    m_latencySleeper.idle(); // a sleep in progress names this swapchain
    // Callers have waited for the device, except after device loss.
    if (m_device && !m_deviceLost)
      completeScreenshot();
    // Sized to the swapchain; made again when a frame next generates.
    m_frameGeneration.reset();
    m_frameGenerationReady = false;
    if (m_frameGenerationPreparation && m_pipelines) {
      m_pipelines->cancel(m_frameGenerationPreparation->job);
      m_obsoleteFrameGeneration.push_back(std::move(m_frameGenerationPreparation));
    }
    m_screenshot.reset(m_device, true);
    shutdownUiVulkan();
    m_hdrOutput.destroy();
    if (!m_commandBuffers.empty()) {
      vkFreeCommandBuffers(m_device, m_commandPool, static_cast<uint32_t>(m_commandBuffers.size()),
                           m_commandBuffers.data());
    }
    m_commandBuffers.clear();
    for (auto handle : m_framebuffers) {
      if (handle) {
        vkDestroyFramebuffer(m_device, handle, nullptr);
      }
    }
    for (auto handle : m_imageViews) {
      if (handle) {
        vkDestroyImageView(m_device, handle, nullptr);
      }
    }
    for (auto handle : m_renderFinished) {
      if (handle) {
        vkDestroySemaphore(m_device, handle, nullptr);
      }
    }
    m_framebuffers.clear();
    m_imageViews.clear();
    m_renderFinished.clear();
    if (m_pathTracer)
      retireTracer(std::move(m_pathTracer), renderer().depthLifetime());
    m_activePreparationKey.clear();
    m_backend = m_targetBackend = 0;
    if (m_preparation && m_pipelines) {
      m_pipelines->cancel(m_preparation->job);
      m_obsoletePreparations.push_back(std::move(m_preparation));
    }
    renderer().destroyTarget();
    if (m_renderPass) {
      pipeline::destroyRenderPass(m_device, m_renderPass, nullptr);
    }
    m_renderPass = VK_NULL_HANDLE;
    if (m_overlayPass)
      pipeline::destroyRenderPass(m_device, m_overlayPass, nullptr);
    m_overlayPass = VK_NULL_HANDLE;
    if (m_resumePass)
      pipeline::destroyRenderPass(m_device, m_resumePass, nullptr);
    m_resumePass = VK_NULL_HANDLE;
    if (m_swapchain) {
      vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    }
    m_swapchain = VK_NULL_HANDLE;
    m_swapchainImages.clear();
    m_swapchainExtent = {};
  }

  bool RemixNativeRuntime::wantsHdr10() const {
    return !m_headless && m_swapchainColorSpace && m_profiles.current().hdr && m_window.hdrEnabled();
  }

  PathTracer::DisplayOutput RemixNativeRuntime::displayOutput() const {
    if (!m_hdrOutput.active())
      return {};
    const auto &settings = m_profiles.current();
    const auto nits = HdrNits{settings.hdrPeakNits, settings.hdrPaperWhiteNits, settings.hdrUiNits}.clamped();
    return {true, nits.peak, nits.paperWhite};
  }

  VkSurfaceFormatKHR RemixNativeRuntime::chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &formats) {
    // HDR10: a 10-bit ST.2084 format; a driver without one presents SDR.
    m_hdr10Requested = wantsHdr10();
    if (m_hdr10Requested) {
      for (VkFormat preferred : HdrOutput::swapchainFormats)
        for (const VkSurfaceFormatKHR &available : formats)
          if (available.format == preferred && available.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
            m_hdrStatus.clear();
            return available;
          }
      m_hdrStatus = "the display offers no 10-bit ST.2084 (HDR10) swapchain format";
    } else {
      m_hdrStatus = m_headless                       ? "headless presentation"
                    : !m_swapchainColorSpace          ? "VK_EXT_swapchain_colorspace is unavailable"
                    : !m_profiles.current().hdr       ? "turned off"
                                                      : "Windows HDR is off for this display";
    }
    // UNORM only: every output shader encodes gamma 2.2 itself, so an sRGB
    // format's hardware encode would apply a second curve.
    const std::array<VkFormat, 2> kPreferredFormats = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    for (VkFormat preferred : kPreferredFormats) {
      for (const VkSurfaceFormatKHR &available : formats) {
        if (available.format == preferred && available.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
          return available;
        }
      }
    }
    if (formats.front().format == VK_FORMAT_UNDEFINED) {
      return {kPreferredFormats[0], VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    }
    for (const VkSurfaceFormatKHR &available : formats)
      if (available.format != VK_FORMAT_B8G8R8A8_SRGB && available.format != VK_FORMAT_R8G8B8A8_SRGB)
        return available;
    return formats.front();
  }

  VkPresentModeKHR RemixNativeRuntime::choosePresentMode(const std::vector<VkPresentModeKHR> &modes) const {
    if (!m_vsync) {
      for (auto preferred : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR}) {
        if (std::find(modes.begin(), modes.end(), preferred) != modes.end()) {
          return preferred;
        }
      }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
  }
} // namespace remix
