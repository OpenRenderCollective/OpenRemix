#include "forward/water/pass.h"
#include "forward/resources.h"
#include "forward/water/shaders/embedded.h"
#include "platform/gpu_frames.h"
#include "runtime/pipelines.h"
#include <algorithm>
#include <array>

namespace remix::forward::water {
  struct Pass::Gpu {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkExtent2D extent{};
    Snapshot snapshot;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer target = VK_NULL_HANDLE;
    VkDescriptorSetLayout inputs = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline surface = VK_NULL_HANDLE, overlay = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    struct Frame {
      VkDescriptorSet set = VK_NULL_HANDLE;
      std::unique_ptr<Buffer> parameters, materials;
    };
    std::array<Frame, gpuFrameCount> frames;
    ~Gpu() {
      if (pool)
        vkDestroyDescriptorPool(device, pool, nullptr);
      for (auto handle : {surface, overlay})
        if (handle)
          pipeline::destroyPipeline(device, handle, nullptr);
      if (layout)
        pipeline::destroyPipelineLayout(device, layout, nullptr);
      if (inputs)
        pipeline::destroyDescriptorSetLayout(device, inputs, nullptr);
      if (target)
        vkDestroyFramebuffer(device, target, nullptr);
      if (pass)
        pipeline::destroyRenderPass(device, pass, nullptr);
    }
    void ensure(std::unique_ptr<Buffer> &buffer, size_t bytes, VkBufferUsageFlags usage) {
      if (!buffer || buffer->capacity < bytes) {
        auto next = std::make_unique<Buffer>();
        next->create(physical, device, std::max<size_t>(bytes, 256), usage);
        buffer = std::move(next);
      }
    }
    void createTarget(VkSampleCountFlagBits samples, Targets views) {
      VkAttachmentDescription attachments[3]{};
      attachments[0] = {.format = VK_FORMAT_R16G16B16A16_SFLOAT,
                        .samples = samples,
                        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                        .initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      attachments[1] = attachments[0];
      attachments[1].format = VK_FORMAT_D32_SFLOAT;
      attachments[1].initialLayout = attachments[1].finalLayout =
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
      attachments[2] = attachments[0];
      attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
      attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      const VkAttachmentReference depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
      const VkAttachmentReference resolve{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      const bool msaa = samples != VK_SAMPLE_COUNT_1_BIT;
      const VkSubpassDescription subpass{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                         .colorAttachmentCount = 1,
                                         .pColorAttachments = &color,
                                         .pResolveAttachments = msaa ? &resolve : nullptr,
                                         .pDepthStencilAttachment = &depth};
      const auto stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
      const auto access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      const VkSubpassDependency dependencies[] = {
          {.srcSubpass = VK_SUBPASS_EXTERNAL,
           .dstSubpass = 0,
           .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
           .dstStageMask = stages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
           .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
           .dstAccessMask = access | VK_ACCESS_SHADER_READ_BIT},
          {.srcSubpass = 0,
           .dstSubpass = VK_SUBPASS_EXTERNAL,
           .srcStageMask = stages,
           .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
           .srcAccessMask =
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
           .dstAccessMask = VK_ACCESS_SHADER_READ_BIT}};
      const VkRenderPassCreateInfo info{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                        .attachmentCount = msaa ? 3u : 2u,
                                        .pAttachments = attachments,
                                        .subpassCount = 1,
                                        .pSubpasses = &subpass,
                                        .dependencyCount = 2,
                                        .pDependencies = dependencies};
      check(pipeline::createRenderPass(device, &info, nullptr, &pass));
      const VkImageView imageViews[] = {views.color, views.depth, views.resolved};
      const VkFramebufferCreateInfo framebuffer{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                                .renderPass = pass,
                                                .attachmentCount = msaa ? 3u : 2u,
                                                .pAttachments = imageViews,
                                                .width = extent.width,
                                                .height = extent.height,
                                                .layers = 1};
      check(vkCreateFramebuffer(device, &framebuffer, nullptr, &target));
    }
    void createPipelines(VkSampleCountFlagBits samples) {
      struct Modules {
        VkDevice device;
        std::vector<VkShaderModule> all;
        VkShaderModule add(const uint32_t *code, size_t bytes) {
          auto module = shaderModule(device, code, bytes);
          all.push_back(module);
          return module;
        }
        ~Modules() {
          for (auto module : all)
            pipeline::destroyShaderModule(device, module, nullptr);
        }
      } modules{device, {}};
      const auto vertex = modules.add(vertShader, sizeof(vertShader));
      const auto fragment = modules.add(fragShader, sizeof(fragShader));
      const auto overlayVertex = modules.add(overlayVertShader, sizeof(overlayVertShader));
      const auto overlayFragment = modules.add(overlayFragShader, sizeof(overlayFragShader));
      const VkVertexInputBindingDescription binding{0, sizeof(scene::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
      const VkVertexInputAttributeDescription attributes[] = {
          {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, position)},
          {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(scene::Vertex, uv)},
          {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(scene::Vertex, color)},
          {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, normal)}};
      const VkPipelineVertexInputStateCreateInfo geometry{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
          .vertexBindingDescriptionCount = 1,
          .pVertexBindingDescriptions = &binding,
          .vertexAttributeDescriptionCount = 4,
          .pVertexAttributeDescriptions = attributes};
      const VkPipelineVertexInputStateCreateInfo fullscreen{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      const VkPipelineInputAssemblyStateCreateInfo assembly{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
          .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
      const VkPipelineViewportStateCreateInfo viewport{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
          .viewportCount = 1,
          .scissorCount = 1};
      const VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
      const VkPipelineDynamicStateCreateInfo dynamic{.sType =
                                                         VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                     .dynamicStateCount = 2,
                                                     .pDynamicStates = states};
      const VkPipelineRasterizationStateCreateInfo raster{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
          .polygonMode = VK_POLYGON_MODE_FILL,
          .cullMode = VK_CULL_MODE_NONE,
          .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
          .lineWidth = 1};
      const VkPipelineMultisampleStateCreateInfo multisample{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = samples};
      auto create = [&](bool additive) {
        const VkPipelineShaderStageCreateInfo stages[] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_VERTEX_BIT,
             .module = additive ? overlayVertex : vertex,
             .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
             .module = additive ? overlayFragment : fragment,
             .pName = "main"}};
        const VkPipelineDepthStencilStateCreateInfo depth{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = !additive,
            .depthWriteEnable = !additive,
            .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
        const VkPipelineColorBlendAttachmentState attachment{.blendEnable = additive,
                                                             .srcColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA,
                                                             .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
                                                             .colorBlendOp = VK_BLEND_OP_ADD,
                                                             .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                                                             .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                                             .alphaBlendOp = VK_BLEND_OP_ADD,
                                                             .colorWriteMask = 15};
        const VkPipelineColorBlendStateCreateInfo blend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &attachment};
        const VkGraphicsPipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                                .stageCount = 2,
                                                .pStages = stages,
                                                .pVertexInputState = additive ? &fullscreen : &geometry,
                                                .pInputAssemblyState = &assembly,
                                                .pViewportState = &viewport,
                                                .pRasterizationState = &raster,
                                                .pMultisampleState = &multisample,
                                                .pDepthStencilState = &depth,
                                                .pColorBlendState = &blend,
                                                .pDynamicState = &dynamic,
                                                .layout = layout,
                                                .renderPass = pass};
        VkPipeline result;
        check(pipeline::createGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &result));
        return result;
      };
      surface = create(false);
      overlay = create(true);
    }
  };
  Pass::Pass() = default;
  Pass::~Pass() = default;
  void Pass::initialize(VkPhysicalDevice physical, VkDevice device, VkExtent2D extent,
                        VkSampleCountFlagBits samples, VkDescriptorSetLayout scene,
                        VkDescriptorSetLayout material, Targets targets) {
    auto next = std::make_unique<Gpu>();
    auto &g = *next;
    g.physical = physical;
    g.device = device;
    g.extent = extent;
    g.snapshot.initialize(physical, device, extent, samples);
    const VkDescriptorType types[] = {
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
    VkDescriptorSetLayoutBinding bindings[11]{};
    for (uint32_t i = 0; i < 11; ++i)
      bindings[i] = {i, types[i], 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    const VkDescriptorSetLayoutCreateInfo setInfo{.sType =
                                                      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                  .bindingCount = 11,
                                                  .pBindings = bindings};
    check(pipeline::createDescriptorSetLayout(device, &setInfo, nullptr, &g.inputs));
    const VkDescriptorSetLayout layouts[] = {scene, material, g.inputs};
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
    const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                .setLayoutCount = 3,
                                                .pSetLayouts = layouts,
                                                .pushConstantRangeCount = 1,
                                                .pPushConstantRanges = &push};
    check(pipeline::createPipelineLayout(device, &layoutInfo, nullptr, &g.layout));
    g.createTarget(samples, targets);
    g.createPipelines(samples);
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 7 * gpuFrameCount},
                                          {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * gpuFrameCount},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * gpuFrameCount}};
    const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .maxSets = gpuFrameCount,
                                              .poolSizeCount = 3,
                                              .pPoolSizes = sizes};
    check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &g.pool));
    for (auto &frame : g.frames) {
      const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                 .descriptorPool = g.pool,
                                                 .descriptorSetCount = 1,
                                                 .pSetLayouts = &g.inputs};
      check(vkAllocateDescriptorSets(device, &allocate, &frame.set));
    }
    m_gpu = std::move(next);
  }
  void Pass::prepare(uint32_t slot, const Parameters &parameters, std::span<const MaterialData> materials,
                     Lighting lighting) {
    auto &g = *m_gpu;
    auto &frame = g.frames.at(slot);
    g.ensure(frame.parameters, sizeof(parameters), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    g.ensure(frame.materials, materials.size_bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    frame.parameters->write(&parameters, sizeof(parameters));
    if (!materials.empty())
      frame.materials->write(materials.data(), materials.size_bytes());
    const auto saved = g.snapshot.output();
    const VkDescriptorImageInfo images[] = {saved.hdr, saved.depth, lighting.dome};
    const VkDescriptorBufferInfo buffers[] = {frame.parameters->descriptor(), frame.materials->descriptor(),
                                              lighting.probes, lighting.sky};
    VkWriteDescriptorSet writes[7]{};
    for (uint32_t i = 0; i < 7; ++i) {
      const bool image = i < 2 || i == 6;
      writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                   .dstSet = frame.set,
                   .dstBinding = i,
                   .descriptorCount = 1,
                   .descriptorType = image              ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                     : i == 2 || i == 5 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                        : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                   .pImageInfo = image ? &images[i == 6 ? 2 : i] : nullptr,
                   .pBufferInfo = image ? nullptr : &buffers[i - 2]};
    }
    vkUpdateDescriptorSets(g.device, 7, writes, 0, nullptr);
    const VkDescriptorImageInfo screen[] = {
        lighting.reflectionDepth.imageView ? lighting.reflectionDepth : saved.depth,
        lighting.reflectionNormals.imageView ? lighting.reflectionNormals : saved.hdr,
        lighting.reflectionColor.imageView ? lighting.reflectionColor : saved.hdr};
    VkWriteDescriptorSet screenWrites[3]{};
    for (uint32_t i = 0; i < 3; ++i)
      screenWrites[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                         .dstSet = frame.set,
                         .dstBinding = 8 + i,
                         .descriptorCount = 1,
                         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         .pImageInfo = &screen[i]};
    vkUpdateDescriptorSets(g.device, 3, screenWrites, 0, nullptr);
  }
  VkDescriptorBufferInfo Pass::parameters(uint32_t slot) const {
    return m_gpu->frames.at(slot).parameters->descriptor();
  }
  void Pass::record(VkCommandBuffer command, uint32_t slot, Snapshot::Inputs inputs,
                    const std::function<void(VkPipeline, VkPipelineLayout)> &draw) {
    auto &g = *m_gpu;
    g.snapshot.record(command, slot, inputs);
    const auto source = inputs.reflections.imageView ? inputs.reflections : g.snapshot.output().hdr;
    const VkWriteDescriptorSet reflection{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .dstSet = g.frames.at(slot).set,
                                          .dstBinding = 7,
                                          .descriptorCount = 1,
                                          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                          .pImageInfo = &source};
    vkUpdateDescriptorSets(g.device, 1, &reflection, 0, nullptr);
    const VkRenderPassBeginInfo begin{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                      .renderPass = g.pass,
                                      .framebuffer = g.target,
                                      .renderArea = {{0, 0}, g.extent}};
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0, 0, float(g.extent.width), float(g.extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, g.extent};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 2, 1, &g.frames[slot].set, 0,
                            nullptr);
    if (inputs.reflections.imageView) {
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.overlay);
      vkCmdDraw(command, 3, 1, 0, 0);
    }
    draw(g.surface, g.layout);
    vkCmdEndRenderPass(command);
  }
  uint64_t Pass::targetBytes() const { return m_gpu ? m_gpu->snapshot.targetBytes() : 0; }
  uint64_t Pass::bufferBytes() const {
    uint64_t bytes = 0;
    if (m_gpu)
      for (const auto &frame : m_gpu->frames)
        for (const auto *buffer : {frame.parameters.get(), frame.materials.get()})
          if (buffer)
            bytes += buffer->allocationBytes;
    return bytes;
  }
} // namespace remix::forward::water
