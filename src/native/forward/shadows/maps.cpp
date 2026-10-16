#include "forward/shadows/maps.h"
#include <algorithm>

namespace remix::forward::shadows {
  Maps::~Maps() {
    for (auto target : m_targets)
      vkDestroyFramebuffer(m_device, target, nullptr);
    for (auto view : m_views)
      vkDestroyImageView(m_device, view, nullptr);
  }
  void Maps::initialize(VkPhysicalDevice physical, VkDevice device, VkRenderPass pass, uint32_t count,
                        uint32_t resolution) {
    m_device = device;
    m_pass = pass;
    m_count = std::min(count, maximumLights);
    m_resolution = m_count ? resolution : 1;
    m_layers = std::max(m_count, 1u) * faces;
    const VkExtent2D extent{m_resolution, m_resolution};
    const auto usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (auto *image : {&m_static, &m_dynamic}) {
      image->create(physical, device, extent, VK_FORMAT_D32_SFLOAT, usage, VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, m_layers);
      for (uint32_t layer = 0; layer < m_layers; ++layer) {
        const VkImageViewCreateInfo viewInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                             .image = image->handle,
                                             .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                             .format = VK_FORMAT_D32_SFLOAT,
                                             .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, layer, 1}};
        VkImageView view;
        check(vkCreateImageView(device, &viewInfo, nullptr, &view));
        m_views.push_back(view);
        const VkFramebufferCreateInfo targetInfo{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                                 .renderPass = pass,
                                                 .attachmentCount = 1,
                                                 .pAttachments = &view,
                                                 .width = m_resolution,
                                                 .height = m_resolution,
                                                 .layers = 1};
        VkFramebuffer target;
        check(vkCreateFramebuffer(device, &targetInfo, nullptr, &target));
        m_targets.push_back(target);
      }
    }
  }
  void Maps::initializeLayouts(VkCommandBuffer command) {
    for (const auto *image : {&m_static, &m_dynamic}) {
      VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = image->handle,
                                   .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, m_layers}};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &barrier);
      const VkClearDepthStencilValue clear{1, 0};
      vkCmdClearDepthStencilImage(command, image->handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                                  &barrier.subresourceRange);
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      barrier.oldLayout = barrier.newLayout;
      barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    m_initialized = true;
  }
  void Maps::record(VkCommandBuffer command, const Selection &selection, const Draw &draw) {
    if (!m_initialized)
      initializeLayouts(command);
    const VkExtent2D extent{m_resolution, m_resolution};
    const VkViewport viewport{0, 0, float(m_resolution), float(m_resolution), 0, 1};
    const VkRect2D scissor{{0, 0}, extent};
    const VkClearValue clear{.depthStencil = {1, 0}};
    for (uint32_t slot = 0; slot < m_count; ++slot) {
      if (!selection.active[slot])
        continue;
      for (bool staticCasters : {true, false}) {
        if (staticCasters && !selection.dirty[slot])
          continue;
        for (uint32_t face = 0; face < faces; ++face) {
          const auto target = m_targets[(staticCasters ? 0 : m_layers) + slot * faces + face];
          const VkRenderPassBeginInfo begin{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                            .renderPass = m_pass,
                                            .framebuffer = target,
                                            .renderArea = {{0, 0}, extent},
                                            .clearValueCount = 1,
                                            .pClearValues = &clear};
          vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
          vkCmdSetViewport(command, 0, 1, &viewport);
          vkCmdSetScissor(command, 0, 1, &scissor);
          draw(slot, face, staticCasters);
          vkCmdEndRenderPass(command);
        }
      }
    }
  }
} // namespace remix::forward::shadows
