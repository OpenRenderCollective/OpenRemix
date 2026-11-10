#include "forward/shadows/sun_maps.h"

namespace remix::forward::shadows {
  SunMaps::~SunMaps() {
    for (auto target : m_targets)
      if (target) vkDestroyFramebuffer(m_device, target, nullptr);
    for (auto view : m_views)
      if (view) vkDestroyImageView(m_device, view, nullptr);
  }
  void SunMaps::initialize(VkPhysicalDevice physical, VkDevice device, VkRenderPass pass, uint32_t resolution) {
    m_device = device;
    m_resolution = resolution;
    const VkExtent2D extent{resolution, resolution};
    constexpr VkFormat format = VK_FORMAT_D32_SFLOAT;
    m_image.create(physical, device, extent, format,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, sunShadowLayers);
    for (uint32_t layer = 0; layer < sunShadowLayers; ++layer) {
      const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
          .image = m_image.handle, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
          .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, layer, 1}};
      check(vkCreateImageView(device, &view, nullptr, &m_views[layer]));
      const VkFramebufferCreateInfo target{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
          .renderPass = pass, .attachmentCount = 1, .pAttachments = &m_views[layer],
          .width = resolution, .height = resolution, .layers = 1};
      check(vkCreateFramebuffer(device, &target, nullptr, &m_targets[layer]));
    }
  }
} // namespace remix::forward::shadows
