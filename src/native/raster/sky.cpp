#include "raster/gpu.h"
#include "raster/shaders/embedded.h"
#include "runtime/pipelines.h"

// The NUMOS sky over the raster target (sky::Renderer::recordRasterComposite):
// inside the target's pass, one fullscreen draw multiplies the target by the
// sky's transmittance and a second adds the sky's light, each the same
// fragment shader (shaders/sky.frag) reading its own image. Nothing here
// exists until a frame draws the sky; the objects go with the target.
namespace remix {
  using namespace raster::detail;
  using namespace raster;

  void RasterRenderer::Gpu::destroySkyComposite() {
    auto &s = skyComposite;
    for (auto &slot : s.sets)
      for (auto &set : slot)
        if (set) {
          vkFreeDescriptorSets(device, pool, 1, &set);
          set = VK_NULL_HANDLE;
        }
    for (auto *pipeline : {&s.multiply, &s.add})
      if (*pipeline)
        pipeline::destroyPipeline(device, std::exchange(*pipeline, VkPipeline{}), nullptr);
    for (auto *shader : {&s.vertex, &s.fragment})
      if (*shader)
        pipeline::destroyShaderModule(device, std::exchange(*shader, VkShaderModule{}), nullptr);
    if (s.layout)
      pipeline::destroyPipelineLayout(device, std::exchange(s.layout, VkPipelineLayout{}), nullptr);
  }

  bool RasterRenderer::Gpu::prepareSkyComposite() {
    auto &s = skyComposite;
    if (!pass)
      return false;
    if (s.multiply && s.add)
      return true;
    if (!s.layout) {
      const VkPipelineLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                               .setLayoutCount = 1,
                                               .pSetLayouts = &setLayout};
      check(pipeline::createPipelineLayout(device, &info, nullptr, &s.layout));
    }
    const auto module = [&](VkShaderModule &out, const uint32_t *code, size_t bytes) {
      if (out)
        return;
      const VkShaderModuleCreateInfo info = {
          .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
      check(pipeline::createShaderModule(device, &info, nullptr, &out));
    };
    module(s.vertex, haloVertShader, sizeof(haloVertShader));
    module(s.fragment, skyCompositeShader, sizeof(skyCompositeShader));
    const VkPipelineShaderStageCreateInfo stages[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                        .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                                        .module = s.vertex,
                                                        .pName = "main"},
                                                       {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                        .module = s.fragment,
                                                        .pName = "main"}};
    const VkPipelineVertexInputStateCreateInfo noInput = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    const VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    const VkPipelineRasterizationStateCreateInfo fullscreen = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1};
    const VkPipelineMultisampleStateCreateInfo samples = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    const VkPipelineDepthStencilStateCreateInfo noDepth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    const VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                      .dynamicStateCount = 2,
                                                      .pDynamicStates = dynamicStates};
    bool ready = true;
    for (const bool add : {false, true}) {
      auto &out = add ? s.add : s.multiply;
      if (out)
        continue;
      // Multiply: target x image. Add: target + image. Alpha is kept.
      const VkPipelineColorBlendAttachmentState blend = {
          .blendEnable = VK_TRUE,
          .srcColorBlendFactor = add ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO,
          .dstColorBlendFactor = add ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_COLOR,
          .colorBlendOp = VK_BLEND_OP_ADD,
          .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
          .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
          .alphaBlendOp = VK_BLEND_OP_ADD,
          .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT};
      const VkPipelineColorBlendStateCreateInfo blending = {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
          .attachmentCount = 1,
          .pAttachments = &blend};
      const VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                                 .stageCount = 2,
                                                 .pStages = stages,
                                                 .pVertexInputState = &noInput,
                                                 .pInputAssemblyState = &assembly,
                                                 .pViewportState = &viewport,
                                                 .pRasterizationState = &fullscreen,
                                                 .pMultisampleState = &samples,
                                                 .pDepthStencilState = &noDepth,
                                                 .pColorBlendState = &blending,
                                                 .pDynamicState = &dynamic,
                                                 .layout = s.layout,
                                                 .renderPass = pass};
      VkPipeline result = VK_NULL_HANDLE;
      const auto status = pipeline::createGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &result);
      if (status == VK_SUCCESS) {
        out = result;
        continue;
      }
      if (result)
        pipeline::destroyPipeline(device, result, nullptr);
      if (status != VK_NOT_READY)
        check(status);
      ready = false;
    }
    return ready;
  }

  bool RasterRenderer::recordSkyComposite(VkCommandBuffer command, const VkDescriptorImageInfo &transmittance,
                                          const VkDescriptorImageInfo &addition) {
    auto &g = *m_gpu;
    auto &s = g.skyComposite;
    if (!transmittance.imageView || !addition.imageView || !g.prepareSkyComposite())
      return false;
    // This slot's sets: its last frame has completed (selectFrame).
    auto &sets = s.sets[g.frameIndex];
    const VkDescriptorImageInfo images[2] = {transmittance, addition};
    VkWriteDescriptorSet writes[2];
    for (int i = 0; i < 2; ++i) {
      if (!sets[i]) {
        const VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                      .descriptorPool = g.pool,
                                                      .descriptorSetCount = 1,
                                                      .pSetLayouts = &g.setLayout};
        check(vkAllocateDescriptorSets(g.device, &allocate, &sets[i]));
      }
      writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                   .dstSet = sets[i],
                   .dstBinding = 0,
                   .descriptorCount = 1,
                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                   .pImageInfo = &images[i]};
    }
    vkUpdateDescriptorSets(g.device, 2, writes, 0, nullptr);
    const VkViewport viewport = {0, 0, float(g.extent.width), float(g.extent.height), 0, 1};
    const VkRect2D scissor = {{0, 0}, g.extent};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    for (int i = 0; i < 2; ++i) {
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, i ? s.add : s.multiply);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 1, &sets[i], 0, nullptr);
      vkCmdDraw(command, 3, 1, 0, 0);
    }
    return true;
  }

  VkImage RasterRenderer::depthImage() const { return m_gpu && m_gpu->depth ? m_gpu->depth->image : VK_NULL_HANDLE; }
} // namespace remix
