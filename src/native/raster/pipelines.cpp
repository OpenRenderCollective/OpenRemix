#include "runtime/pipelines.h"
#include "raster/gpu.h"
#include "forward/lighting.h"

namespace remix {
  using namespace scene;
  using namespace raster::detail;
  namespace {
    uint64_t pipelineKey(const Draw &draw) {
      const auto &b = draw.blend;
      return uint64_t(b.src) | uint64_t(b.dst) << 4 | uint64_t(b.op) << 8 | uint64_t(b.srcAlpha) << 11 |
             uint64_t(b.dstAlpha) << 15 | uint64_t(b.alphaOp) << 19 | uint64_t(b.mask) << 22 |
             uint64_t(b.enabled) << 26 | uint64_t(draw.doubleSided) << 27 | uint64_t(draw.depth) << 28 |
             uint64_t(draw.depth && (draw.depthWriteOverride ? draw.depthWrite : (!b.enabled || draw.ui)))
                 << 29 |
             uint64_t(draw.depthCompare) << 30 | uint64_t(draw.depthBias) << 33 | uint64_t(draw.wireframe) << 34;
    }
  } // namespace

  bool RasterRenderer::preparePipelines(bool tracing, bool forward, DrawOwner owner) {
    if (forward) applyViewModel(1.f);
    bool ready = true;
    // Masked overlays (raster/mask.cpp): their coverage machinery and their
    // pipeline variants are compiled on first use, as for halos. A frame never
    // waits for them: until they are ready (and if the machinery cannot be
    // created) prepare() leaves masked draws out.
    int masks = -1;
    const auto masksReady = [&] {
      if (masks < 0) {
        try {
          masks = m_gpu->prepareMask();
        } catch (VkResult result) {
          if (result == VK_ERROR_DEVICE_LOST)
            throw;
          m_gpu->mask.failed = true;
          masks = 0;
        }
      }
      return masks > 0;
    };
    if (!m_masks.empty())
      masksReady();
    // Sprite overlays (raster/sprite.cpp) take their own stages; a frame waits
    // for them like for any unmasked draw. If they cannot be created the
    // draws are plain.
    int sprites = -1;
    const auto spriteStages = [&](const scene::Draw &draw) {
      if (!draw.spriteFlags || !draw.blend.enabled || draw.maskGroup || draw.cutGroup || draw.ui)
        return false;
      if (sprites < 0) {
        try {
          sprites = m_gpu->prepareSprite();
        } catch (VkResult result) {
          if (result == VK_ERROR_DEVICE_LOST)
            throw;
          m_gpu->sprite.failed = true;
          sprites = 0;
        }
      }
      return sprites > 0;
    };
    // With the path tracer the raster draws are overlays and effects (a
    // muzzle flash, an impact, the HUD): a frame never waits for a variant
    // of theirs to compile. Waiting froze the picture for the length of each
    // compile, about 100 ms, and the first shot of a session needs several.
    // prepare() leaves such a draw out until its pipeline is ready. The raster
    // backend draws the scene itself and waits, as before.
    const auto request = [&](const scene::Draw &draw, bool masked, bool wait = true) {
      try {
        m_gpu->pipeline(draw, masked, !masked && spriteStages(draw));
      } catch (VkResult result) {
        if (result != VK_NOT_READY)
          throw;
        ready = ready && (!wait || tracing);
      }
    };
    // The variants prepare() can choose for a draw: a masked draw's is tested
    // against its group's coverage; an OUTSIDE draw is drawn plainly while its
    // group has no sources. A rasterized CUT instance is a scene draw, drawn
    // plainly (waited for) until its masked variant and coverage exist.
    const auto variants = [&](const scene::Draw &draw) {
      if (!draw.maskGroup && draw.cutGroup) {
        request(draw, false);
        if (draw.camera != REMIXAPI_CAMERA_TYPE_SKY && !m_masks.empty() && masksReady())
          request(draw, true, false);
        return;
      }
      if (!draw.maskGroup)
        return request(draw, false);
      if (draw.camera == REMIXAPI_CAMERA_TYPE_SKY || !masksReady())
        return;
      if (draw.maskOutside)
        request(draw, false, false);
      request(draw, true, false);
    };
    for (const auto &draw : m_draws)
      if (draw.rasterVisible && (!tracing || !draw.traced()) &&
          (!forward || !(owner ? owner(draw) : forward::accepts(draw)))) {
        if (tracing && draw.depthCompare == 2) {
          auto adjusted = draw;
          adjusted.depthCompare = 3;
          adjusted.depthBias = true;
          variants(adjusted);
        } else
          variants(draw);
      }
    // These draws are appended after the depth adjustment in prepare().
    const auto overlays = [&](const auto &draws) {
      for (const auto &draw : draws)
        variants(draw);
    };
    overlays(m_overlay);
    overlays(m_uiDraws);
    // Halo outlines: compiled on first use. A frame never waits for them; it
    // is drawn without halos until they are ready (prepare()).
    if (!m_halos.empty()) {
      try {
        m_gpu->prepareHalo();
      } catch (VkResult result) {
        // Halos are decoration: a failure to create them leaves frames without.
        if (result == VK_ERROR_DEVICE_LOST)
          throw;
        m_gpu->halo.failed = true;
      }
    }
    if (m_tint[3] > 0) {
      scene::Draw tint;
      tint.ui = true;
      tint.depth = false;
      tint.doubleSided = true;
      tint.blend.enabled = true;
      request(tint, false);
    }
    return ready;
  }

  VkPipeline RasterRenderer::Gpu::pipeline(const Draw &draw, bool masked, bool spriteStages) {
    const bool sky = skyPass && draw.camera == REMIXAPI_CAMERA_TYPE_SKY;
    const uint64_t key =
        pipelineKey(draw) | uint64_t(sky) << 34 | uint64_t(masked) << 35 | uint64_t(spriteStages) << 36;
    if (auto it = pipelines.find(key); it != pipelines.end()) {
      return it->second;
    }
    if (pipelines.size() >= 256) {
      throw VK_ERROR_TOO_MANY_OBJECTS;
    }
    struct Timed {
      double &total;
      std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
      ~Timed() { total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count(); }
    } timed{createPipelineMs};
    ++pipelinesCreated;
    const VkPipelineShaderStageCreateInfo stages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = spriteStages ? sprite.vertex : vertex,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = masked ? mask.overlayFragment : spriteStages ? sprite.fragment : fragment,
         .pName = "main"}};
    // Sprite overlays: the mesh's sprite vertices as a second binding.
    const VkVertexInputBindingDescription bindings[] = {{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
                                                        {1, sizeof(SpriteVertex), VK_VERTEX_INPUT_RATE_VERTEX}};
    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
        {3, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SpriteVertex, frame)},
        {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteVertex, sequence)},
        {5, 1, VK_FORMAT_R32_SFLOAT, offsetof(SpriteVertex, weight2)}};
    const VkPipelineVertexInputStateCreateInfo input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = spriteStages ? 2u : 1u,
        .pVertexBindingDescriptions = bindings,
        .vertexAttributeDescriptionCount = spriteStages ? 6u : 3u,
        .pVertexAttributeDescriptions = attributes};
    const VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    const VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1};
    const VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = draw.wireframe && fillModeNonSolid ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL,
        .cullMode = VkCullModeFlags(draw.doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT),
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = draw.depthBias,
        .depthBiasConstantFactor = -1.f,
        .depthBiasSlopeFactor = -1.f,
        .lineWidth = 1};
    const VkPipelineMultisampleStateCreateInfo samples = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    // SKY instances skip the depth test in the shared main pass (their camera
    // is not the world's). The path tracer's sky pass has its own depth
    // buffer, so there the 3D skybox's geometry occludes itself.
    const bool depth = draw.depth || sky;
    const VkPipelineDepthStencilStateCreateInfo depthState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = depth,
        .depthWriteEnable =
            depth && (draw.depthWriteOverride ? draw.depthWrite : (!draw.blend.enabled || draw.ui)),
        .depthCompareOp = VkCompareOp(draw.depthCompare)};
    const auto &b = draw.blend;
    const VkPipelineColorBlendAttachmentState blend = {.blendEnable = b.enabled,
                                                       .srcColorBlendFactor = VkBlendFactor(b.src),
                                                       .dstColorBlendFactor = VkBlendFactor(b.dst),
                                                       .colorBlendOp = VkBlendOp(b.op),
                                                       .srcAlphaBlendFactor = VkBlendFactor(b.srcAlpha),
                                                       .dstAlphaBlendFactor = VkBlendFactor(b.dstAlpha),
                                                       .alphaBlendOp = VkBlendOp(b.alphaOp),
                                                       .colorWriteMask = b.mask};
    const VkPipelineColorBlendStateCreateInfo blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend};
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                            VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    const VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 3,
        .pDynamicStates = dynamicStates};
    const VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                               .stageCount = 2,
                                               .pStages = stages,
                                               .pVertexInputState = &input,
                                               .pInputAssemblyState = &assembly,
                                               .pViewportState = &viewport,
                                               .pRasterizationState = &raster,
                                               .pMultisampleState = &samples,
                                               .pDepthStencilState = &depthState,
                                               .pColorBlendState = &blending,
                                               .pDynamicState = &dynamic,
                                               .layout = masked ? mask.layout : spriteStages ? sprite.layout : layout,
                                               .renderPass = sky ? skyPass : pass};
    VkPipeline result = VK_NULL_HANDLE;
    const auto status = pipeline::createGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &info, nullptr, &result);
    if (status != VK_SUCCESS) {
      if (result) {
        pipeline::destroyPipeline(device, result, nullptr);
      }
      check(status);
    }
    try {
      pipelines.emplace(key, result);
    } catch (...) {
      pipeline::destroyPipeline(device, result, nullptr);
      throw;
    }
    return result;
  }

} // namespace remix
