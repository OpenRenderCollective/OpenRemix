#include "runtime/hdr_output.h"
#include "upscale/shaders/embedded.h"

namespace remix {
  namespace {
    void check(VkResult result) {
      if (result != VK_SUCCESS)
        throw result;
    }
  } // namespace

  // Plain Vulkan objects throughout: created on the thread presenting (which
  // the pipeline service answers with VK_NOT_READY until a worker compiled),
  // and a pass this small compiles at once.

  void HdrOutput::create(VkPhysicalDevice physical, VkDevice device, VkFormat swapchainFormat, VkExtent2D extent,
                         const std::vector<VkImageView> &swapchainViews, uint32_t slots, VkRenderPass mainPass,
                         VkImageView depth) {
    destroy();
    m_device = device;
    m_extent = extent;
    try {
      VkPhysicalDeviceMemoryProperties memory{};
      vkGetPhysicalDeviceMemoryProperties(physical, &memory);
      // The encode pass: the swapchain image is written whole.
      const VkAttachmentDescription attachment{.format = swapchainFormat,
                                               .samples = VK_SAMPLE_COUNT_1_BIT,
                                               .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                               .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                               .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                               .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                               .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                               .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
      const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      const VkSubpassDescription subpass{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                         .colorAttachmentCount = 1,
                                         .pColorAttachments = &color};
      // After the image's acquisition (its semaphore waits at colour output)
      // and the frame's earlier uses of it (transfers, frame generation's
      // capture of the frame without HUD), and the back buffer's writes.
      const VkSubpassDependency dependency{
          .srcSubpass = VK_SUBPASS_EXTERNAL,
          .dstSubpass = 0,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                           VK_ACCESS_TRANSFER_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT};
      const VkRenderPassCreateInfo pass{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                        .attachmentCount = 1,
                                        .pAttachments = &attachment,
                                        .subpassCount = 1,
                                        .pSubpasses = &subpass,
                                        .dependencyCount = 1,
                                        .pDependencies = &dependency};
      check(vkCreateRenderPass(device, &pass, nullptr, &m_pass));
      for (const auto view : swapchainViews) {
        const VkFramebufferCreateInfo framebuffer{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                                  .renderPass = m_pass,
                                                  .attachmentCount = 1,
                                                  .pAttachments = &view,
                                                  .width = extent.width,
                                                  .height = extent.height,
                                                  .layers = 1};
        m_swapchainFramebuffers.push_back(VK_NULL_HANDLE);
        check(vkCreateFramebuffer(device, &framebuffer, nullptr, &m_swapchainFramebuffers.back()));
      }
      const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                        .magFilter = VK_FILTER_NEAREST,
                                        .minFilter = VK_FILTER_NEAREST,
                                        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
      check(vkCreateSampler(device, &sampler, nullptr, &m_sampler));
      const VkDescriptorSetLayoutBinding binding{.binding = 0,
                                                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                 .descriptorCount = 1,
                                                 .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
      const VkDescriptorSetLayoutCreateInfo setLayout{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                      .bindingCount = 1,
                                                      .pBindings = &binding};
      check(vkCreateDescriptorSetLayout(device, &setLayout, nullptr, &m_setLayout));
      const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, slots};
      const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                            .maxSets = slots,
                                            .poolSizeCount = 1,
                                            .pPoolSizes = &size};
      check(vkCreateDescriptorPool(device, &pool, nullptr, &m_pool));
      const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float)};
      const VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                              .setLayoutCount = 1,
                                              .pSetLayouts = &m_setLayout,
                                              .pushConstantRangeCount = 1,
                                              .pPushConstantRanges = &push};
      check(vkCreatePipelineLayout(device, &layout, nullptr, &m_layout));

      m_backBuffers.resize(slots);
      for (auto &b : m_backBuffers) {
        const VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                      .imageType = VK_IMAGE_TYPE_2D,
                                      .format = backBufferFormat,
                                      .extent = {extent.width, extent.height, 1},
                                      .mipLevels = 1,
                                      .arrayLayers = 1,
                                      .samples = VK_SAMPLE_COUNT_1_BIT,
                                      .tiling = VK_IMAGE_TILING_OPTIMAL,
                                      // Drawn to, encoded from, and copied by the frame
                                      // snapshot, the host passes and screenshots.
                                      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        check(vkCreateImage(device, &image, nullptr, &b.image));
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, b.image, &requirements);
        uint32_t type = 0;
        for (; type < memory.memoryTypeCount; ++type)
          if ((requirements.memoryTypeBits & (1u << type)) &&
              (memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            break;
        if (type == memory.memoryTypeCount)
          throw VK_ERROR_FEATURE_NOT_PRESENT;
        const VkMemoryAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                              .allocationSize = requirements.size,
                                              .memoryTypeIndex = type};
        check(vkAllocateMemory(device, &allocation, nullptr, &b.memory));
        check(vkBindImageMemory(device, b.image, b.memory, 0));
        const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                         .image = b.image,
                                         .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                         .format = backBufferFormat,
                                         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        check(vkCreateImageView(device, &view, nullptr, &b.view));
        const VkImageView views[] = {b.view, depth};
        const VkFramebufferCreateInfo framebuffer{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                                  .renderPass = mainPass,
                                                  .attachmentCount = 2,
                                                  .pAttachments = views,
                                                  .width = extent.width,
                                                  .height = extent.height,
                                                  .layers = 1};
        check(vkCreateFramebuffer(device, &framebuffer, nullptr, &b.framebuffer));
        const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                   .descriptorPool = m_pool,
                                                   .descriptorSetCount = 1,
                                                   .pSetLayouts = &m_setLayout};
        check(vkAllocateDescriptorSets(device, &allocate, &b.set));
        const VkDescriptorImageInfo info{m_sampler, b.view, VK_IMAGE_LAYOUT_GENERAL};
        const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = b.set,
                                         .dstBinding = 0,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                         .pImageInfo = &info};
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
      }

      const VkPipelineLayoutCreateInfo uiLayout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      check(vkCreatePipelineLayout(device, &uiLayout, nullptr, &m_uiLayout));

      // The fullscreen triangle, the encode and the UI scale.
      VkShaderModule modules[3]{};
      try {
        const uint32_t *codes[] = {upscale::vertShader, upscale::hdr10Shader, upscale::hdrUiShader};
        const size_t sizes[] = {sizeof(upscale::vertShader), sizeof(upscale::hdr10Shader),
                                sizeof(upscale::hdrUiShader)};
        for (unsigned i = 0; i < 3; ++i) {
          const VkShaderModuleCreateInfo module{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                .codeSize = sizes[i],
                                                .pCode = codes[i]};
          check(vkCreateShaderModule(device, &module, nullptr, &modules[i]));
        }
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (unsigned i = 0; i < 2; ++i)
          stages[i] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                       .stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT,
                       .module = modules[i],
                       .pName = "main"};
        const VkPipelineVertexInputStateCreateInfo vertex{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        const VkPipelineInputAssemblyStateCreateInfo assembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        const VkViewport viewport{0, 0, float(extent.width), float(extent.height), 0, 1};
        const VkRect2D scissor{{0, 0}, extent};
        const VkPipelineViewportStateCreateInfo viewportState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .pViewports = &viewport,
            .scissorCount = 1,
            .pScissors = &scissor};
        const VkPipelineRasterizationStateCreateInfo raster{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .lineWidth = 1};
        const VkPipelineMultisampleStateCreateInfo samples{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        const VkPipelineColorBlendAttachmentState blendAttachment{.colorWriteMask = 15};
        const VkPipelineColorBlendStateCreateInfo blend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &blendAttachment};
        const VkGraphicsPipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                                .stageCount = 2,
                                                .pStages = stages,
                                                .pVertexInputState = &vertex,
                                                .pInputAssemblyState = &assembly,
                                                .pViewportState = &viewportState,
                                                .pRasterizationState = &raster,
                                                .pMultisampleState = &samples,
                                                .pColorBlendState = &blend,
                                                .layout = m_layout,
                                                .renderPass = m_pass};
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &m_pipeline));

        // The UI scale, in the frame's pass: colour = target x constant,
        // alpha kept; no depth test.
        stages[1].module = modules[2];
        const VkPipelineColorBlendAttachmentState scaleAttachment{.blendEnable = VK_TRUE,
                                                                  .srcColorBlendFactor = VK_BLEND_FACTOR_ZERO,
                                                                  .dstColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR,
                                                                  .colorBlendOp = VK_BLEND_OP_ADD,
                                                                  .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                                                                  .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                                                  .alphaBlendOp = VK_BLEND_OP_ADD,
                                                                  .colorWriteMask = 15};
        const VkPipelineColorBlendStateCreateInfo scaleBlend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &scaleAttachment};
        const VkPipelineDepthStencilStateCreateInfo depthState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthCompareOp = VK_COMPARE_OP_ALWAYS,
            .maxDepthBounds = 1};
        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_BLEND_CONSTANTS};
        const VkPipelineDynamicStateCreateInfo dynamic{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                       .dynamicStateCount = 1,
                                                       .pDynamicStates = dynamicStates};
        auto scale = info;
        scale.pColorBlendState = &scaleBlend;
        scale.pDepthStencilState = &depthState;
        scale.pDynamicState = &dynamic;
        scale.layout = m_uiLayout;
        scale.renderPass = mainPass;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &scale, nullptr, &m_uiPipeline));
      } catch (...) {
        for (auto module : modules)
          if (module)
            vkDestroyShaderModule(device, module, nullptr);
        throw;
      }
      for (auto module : modules)
        vkDestroyShaderModule(device, module, nullptr);
    } catch (...) {
      destroy();
      throw;
    }
  }

  void HdrOutput::destroy() {
    if (!m_device)
      return;
    for (auto pipeline : {m_pipeline, m_uiPipeline})
      if (pipeline)
        vkDestroyPipeline(m_device, pipeline, nullptr);
    for (auto &b : m_backBuffers) {
      if (b.framebuffer)
        vkDestroyFramebuffer(m_device, b.framebuffer, nullptr);
      if (b.view)
        vkDestroyImageView(m_device, b.view, nullptr);
      if (b.image)
        vkDestroyImage(m_device, b.image, nullptr);
      if (b.memory)
        vkFreeMemory(m_device, b.memory, nullptr);
    }
    for (auto framebuffer : m_swapchainFramebuffers)
      if (framebuffer)
        vkDestroyFramebuffer(m_device, framebuffer, nullptr);
    if (m_pool)
      vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    for (auto layout : {m_layout, m_uiLayout})
      if (layout)
        vkDestroyPipelineLayout(m_device, layout, nullptr);
    if (m_setLayout)
      vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
    if (m_sampler)
      vkDestroySampler(m_device, m_sampler, nullptr);
    if (m_pass)
      vkDestroyRenderPass(m_device, m_pass, nullptr);
    m_backBuffers.clear();
    m_swapchainFramebuffers.clear();
    m_pipeline = m_uiPipeline = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_layout = m_uiLayout = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_pass = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
  }

  void HdrOutput::prepare(VkCommandBuffer command, uint32_t slot) {
    auto &b = m_backBuffers[slot];
    if (b.initialized)
      return;
    b.initialized = true;
    const VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                       .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                                        VK_ACCESS_TRANSFER_WRITE_BIT,
                                       .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                       .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .image = b.image,
                                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
  }

  void HdrOutput::scaleUi(VkCommandBuffer command, float factor) {
    // The whole target (the pipeline's viewport is static).
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_uiPipeline);
    const float constants[] = {factor, factor, factor, 1};
    vkCmdSetBlendConstants(command, constants);
    vkCmdDraw(command, 3, 1, 0, 0);
  }

  void HdrOutput::encode(VkCommandBuffer command, uint32_t slot, uint32_t imageIndex, float paperWhiteNits) {
    const auto &b = m_backBuffers[slot];
    // The frame's draws and copies into the back buffer, then its reads here.
    const VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                       .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                                        VK_ACCESS_TRANSFER_WRITE_BIT,
                                       .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                                       .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                                       .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .image = b.image,
                                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    const VkRenderPassBeginInfo pass{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                     .renderPass = m_pass,
                                     .framebuffer = m_swapchainFramebuffers[imageIndex],
                                     .renderArea = {{0, 0}, m_extent}};
    vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &b.set, 0, nullptr);
    vkCmdPushConstants(command, m_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(paperWhiteNits), &paperWhiteNits);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    // Later passes of the frame (the snapshot capture, the developer UI)
    // write the back buffer again: after these reads.
    const VkImageMemoryBarrier after{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                     .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
                                     .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                                      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                                      VK_ACCESS_TRANSFER_READ_BIT,
                                     .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                                     .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .image = b.image,
                                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &after);
  }
} // namespace remix
