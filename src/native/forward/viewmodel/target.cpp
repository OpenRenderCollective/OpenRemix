#include "forward/viewmodel/target.h"
#include "runtime/pipelines.h"

namespace remix::forward::viewmodel {
  Target::~Target() {
    if (m_framebuffer) vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
    if (m_pass) pipeline::destroyRenderPass(m_device, m_pass, nullptr);
  }
  void Target::initialize(VkPhysicalDevice physical, VkDevice device, VkExtent2D extent,
                          VkSampleCountFlagBits samples) {
    m_physical = physical;
    m_device = device;
    m_extent = extent;
    m_samples = samples;
    VkAttachmentDescription attachments[3]{};
    attachments[0] = {.format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .samples = samples, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    attachments[1] = attachments[0];
    attachments[1].format = VK_FORMAT_D32_SFLOAT;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[2] = attachments[0];
    attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference resolve{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const bool msaa = samples != VK_SAMPLE_COUNT_1_BIT;
    const VkSubpassDescription subpass{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &color,
        .pResolveAttachments = msaa ? &resolve : nullptr, .pDepthStencilAttachment = &depth};
    const auto attachmentStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkSubpassDependency dependencies[] = {
        {.srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
         .srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | attachmentStages,
         .dstStageMask = attachmentStages,
         .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT},
        {.srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
         .srcStageMask = attachmentStages, .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT}};
    const VkRenderPassCreateInfo info{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = msaa ? 3u : 2u, .pAttachments = attachments,
        .subpassCount = 1, .pSubpasses = &subpass,
        .dependencyCount = 2, .pDependencies = dependencies};
    check(pipeline::createRenderPass(device, &info, nullptr, &m_pass));
  }
  void Target::ensure() {
    if (m_framebuffer) return;
    auto image = [&](VkFormat format, VkImageUsageFlags usage, VkSampleCountFlagBits samples,
                     VkImageAspectFlags aspect) {
      auto result = std::make_unique<Image>();
      result->create(m_physical, m_device, m_extent, format, usage, samples, aspect);
      return result;
    };
    const auto colorUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    m_color = image(VK_FORMAT_R16G16B16A16_SFLOAT, colorUsage, m_samples, VK_IMAGE_ASPECT_COLOR_BIT);
    m_depth = image(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                    m_samples, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (m_samples != VK_SAMPLE_COUNT_1_BIT)
      m_resolved = image(VK_FORMAT_R16G16B16A16_SFLOAT, colorUsage, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    const VkImageView views[] = {m_color->view, m_depth->view, m_resolved ? m_resolved->view : VK_NULL_HANDLE};
    const VkFramebufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = m_pass, .attachmentCount = m_resolved ? 3u : 2u, .pAttachments = views,
        .width = m_extent.width, .height = m_extent.height, .layers = 1};
    check(vkCreateFramebuffer(m_device, &info, nullptr, &m_framebuffer));
  }
  void Target::record(VkCommandBuffer command, const std::function<void()> &draw) {
    const VkClearValue clear[] = {{.color = {{0, 0, 0, 0}}}, {.depthStencil = {1, 0}}};
    const VkRenderPassBeginInfo info{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = m_pass, .framebuffer = m_framebuffer, .renderArea = {{0, 0}, m_extent},
        .clearValueCount = 2, .pClearValues = clear};
    vkCmdBeginRenderPass(command, &info, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0, 0, float(m_extent.width), float(m_extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, m_extent};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    draw();
    vkCmdEndRenderPass(command);
  }
  uint64_t Target::bytes() const {
    return (m_color ? m_color->bytes : 0) + (m_depth ? m_depth->bytes : 0) +
           (m_resolved ? m_resolved->bytes : 0);
  }
  SunShadow::~SunShadow() {
    for (auto framebuffer : m_framebuffers)
      if (framebuffer) vkDestroyFramebuffer(m_device, framebuffer, nullptr);
    for (auto layer : m_layers)
      if (layer) vkDestroyImageView(m_device, layer, nullptr);
  }
  void SunShadow::initialize(VkPhysicalDevice physical, VkDevice device, VkRenderPass pass,
                              uint32_t resolution) {
    m_device = device;
    m_pass = pass;
    m_resolution = resolution;
    m_depth.create(physical, device, {resolution, resolution}, VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, 2, 1, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    for (uint32_t layer = 0; layer < m_layers.size(); ++layer) {
      const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
          .image = m_depth.handle, .viewType = VK_IMAGE_VIEW_TYPE_2D,
          .format = VK_FORMAT_D32_SFLOAT,
          .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, layer, 1}};
      check(vkCreateImageView(device, &view, nullptr, &m_layers[layer]));
      const VkFramebufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
          .renderPass = pass, .attachmentCount = 1, .pAttachments = &m_layers[layer],
          .width = resolution, .height = resolution, .layers = 1};
      check(vkCreateFramebuffer(device, &info, nullptr, &m_framebuffers[layer]));
    }
  }
  void SunShadow::record(VkCommandBuffer command, const std::function<void(uint32_t)> &draw) {
    const VkClearValue clear{.depthStencil = {1, 0}};
    for (uint32_t layer = 0; layer < m_framebuffers.size(); ++layer) {
      const VkRenderPassBeginInfo info{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
          .renderPass = m_pass, .framebuffer = m_framebuffers[layer],
          .renderArea = {{0, 0}, {m_resolution, m_resolution}}, .clearValueCount = 1, .pClearValues = &clear};
      vkCmdBeginRenderPass(command, &info, VK_SUBPASS_CONTENTS_INLINE);
      const VkViewport viewport{0, 0, float(m_resolution), float(m_resolution), 0, 1};
      const VkRect2D scissor{{0, 0}, {m_resolution, m_resolution}};
      vkCmdSetViewport(command, 0, 1, &viewport);
      vkCmdSetScissor(command, 0, 1, &scissor);
      draw(layer);
      vkCmdEndRenderPass(command);
    }
  }
} // namespace remix::forward::viewmodel
