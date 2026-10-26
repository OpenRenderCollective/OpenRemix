#include "raster/gpu.h"
#include "raster/shaders/embedded.h"
#include "runtime/pipelines.h"

// Halo outlines (remixapi_InstanceInfoHaloEXT), after Garry's Mod's halo.Add.
// A group's sources are drawn as coverage into a mask at target resolution,
// depth-tested against the frame's depth attachment unless the halo ignores
// depth; the mask is blurred by a separable Gaussian, twice (the reference's
// render.BlurRenderTarget); the blurred coverage, in the halo's colour, is
// added to the frame, or multiplied into it, where the mask is empty.
//
// Each group runs mask, blur and composite in turn on the same three targets,
// so groups composite in order and a later halo may glow over an earlier
// group's silhouette, as in the reference. All passes are confined to the
// pixels the group can reach (its sources' screen bounds plus the blur).
namespace remix {
  using namespace scene;
  using namespace raster::detail;
  using namespace raster;

  namespace {
    constexpr VkFormat coverageFormat = VK_FORMAT_R8_UNORM;
    constexpr int blurIterations = 2;
    constexpr int maxTaps = 16;

    struct BlurPush {
      float area[4];
      float texel[2];
      float direction[2];
      float sigma, stride;
      int32_t taps;
      uint32_t padding;
    };
    struct CompositePush {
      float color[4];
      uint32_t flags;
      uint32_t padding[3];
    };
    static_assert(sizeof(BlurPush) == 48 && sizeof(CompositePush) == 32);

    // One blur pass along an axis. blur is the reference's blur size, in pixels
    // at 1080p output height: its blur shader spreads nine taps over +-4 sizes,
    // a standard deviation of about two sizes per iteration.
    constexpr float sigmaPerBlur = 2;
    struct Kernel {
      float sigma, stride;
      int32_t taps;
      float reach; // texels the pass spreads coverage by
    };
    Kernel kernel(float blur, uint32_t height) {
      Kernel k;
      k.sigma = std::max(sigmaPerBlur * blur * float(height) / 1080.f, 1e-3f);
      const float radius = std::max(1.f, std::ceil(2.5f * k.sigma));
      k.taps = std::min(maxTaps, int32_t(std::ceil(radius / 2)));
      // Past maxTaps bilinear pairs the taps spread out instead of multiplying.
      k.stride = std::max(1.f, radius / float(2 * k.taps));
      k.reach = float(2 * k.taps) * k.stride;
      return k;
    }

    VkShaderModule module(VkDevice device, const uint32_t *code, size_t bytes) {
      const VkShaderModuleCreateInfo info = {
          .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
      VkShaderModule out = VK_NULL_HANDLE;
      check(pipeline::createShaderModule(device, &info, nullptr, &out));
      return out;
    }
  } // namespace

  void RasterRenderer::Gpu::destroyCoverageTarget(Halo::Target &target) {
    if (target.set)
      vkFreeDescriptorSets(device, pool, 1, &target.set);
    if (target.framebuffer)
      vkDestroyFramebuffer(device, target.framebuffer, nullptr);
    if (target.view)
      vkDestroyImageView(device, target.view, nullptr);
    if (target.image)
      vkDestroyImage(device, target.image, nullptr);
    if (target.memory)
      vkFreeMemory(device, target.memory, nullptr);
    target = {};
  }

  void RasterRenderer::Gpu::destroyHaloTargets() {
    for (auto &target : halo.targets)
      destroyCoverageTarget(target);
  }

  void RasterRenderer::Gpu::destroyHalo() {
    auto &h = halo;
    destroyHaloTargets();
    for (auto *pipelines : {&h.mask[0], &h.mask[1], &h.blur, &h.composite[0], &h.composite[1]}) {
      if (*pipelines)
        pipeline::destroyPipeline(device, *pipelines, nullptr);
      *pipelines = VK_NULL_HANDLE;
    }
    for (auto *shader : {&h.maskFragment, &h.vertex, &h.blurFragment, &h.compositeFragment}) {
      if (*shader)
        pipeline::destroyShaderModule(device, *shader, nullptr);
      *shader = VK_NULL_HANDLE;
    }
    for (auto *pass : {&h.maskPass, &h.loadPass, &h.earlyPass, &h.earlyLoadPass, &h.blurPass}) {
      if (*pass)
        pipeline::destroyRenderPass(device, *pass, nullptr);
      *pass = VK_NULL_HANDLE;
    }
    if (h.layout)
      pipeline::destroyPipelineLayout(device, h.layout, nullptr);
    h.layout = VK_NULL_HANDLE;
    if (h.sampler)
      pipeline::destroySampler(device, h.sampler, nullptr);
    h.sampler = VK_NULL_HANDLE;
    h.ready = h.coverageReady = h.failed = false;
    h.groups.clear();
    h.commands.clear();
  }

  namespace {
    // Fixed state shared by the coverage and fullscreen pipelines.
    struct PipelineState {
      VkPipelineInputAssemblyStateCreateInfo assembly = {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
          .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
      VkPipelineViewportStateCreateInfo viewport = {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
      VkPipelineMultisampleStateCreateInfo samples = {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
          .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
      VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
      VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                  .dynamicStateCount = 2,
                                                  .pDynamicStates = dynamicStates};
      VkPipelineShaderStageCreateInfo stages[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                    .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                                    .pName = "main"},
                                                   {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                    .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                    .pName = "main"}};
    };
    // A pipeline that is still compiling is requested again by the next frame.
    // False while it is not ready.
    bool create(VkDevice device, const VkGraphicsPipelineCreateInfo &info, VkPipeline &out) {
      if (out)
        return true;
      VkPipeline result = VK_NULL_HANDLE;
      const auto status = pipeline::createGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &result);
      if (status == VK_SUCCESS) {
        out = result;
        return true;
      }
      if (result)
        pipeline::destroyPipeline(device, result, nullptr);
      if (status != VK_NOT_READY)
        check(status);
      return false;
    }
  } // namespace

  bool RasterRenderer::Gpu::coveragePipeline(VkPipeline &out, VkShaderModule fragmentModule,
                                             VkPipelineLayout pipelineLayout, bool tested) {
    PipelineState state;
    // Silhouette: the raster vertex stage and material alpha test. Both faces,
    // so the silhouette does not depend on winding or mirroring.
    state.stages[0].module = vertex;
    state.stages[1].module = fragmentModule;
    const VkVertexInputBindingDescription binding = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)}};
    const VkPipelineVertexInputStateCreateInfo input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 3,
        .pVertexAttributeDescriptions = attributes};
    // The source is usually also in the scene, at the same depth: the frame's
    // depth comes from another pipeline (the tracer's primary surface, at a
    // jittered sample without an upscaler), so the test is biased toward the
    // camera by a few depth steps and by the surface's slope over a pixel.
    const VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = VkBool32(tested),
        .depthBiasConstantFactor = depthFormat == VK_FORMAT_D32_SFLOAT ? -16.f : -2.f,
        .depthBiasSlopeFactor = -2.f,
        .lineWidth = 1};
    const VkPipelineDepthStencilStateCreateInfo depthState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VkBool32(tested),
        .depthWriteEnable = VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
    const VkPipelineColorBlendAttachmentState blend = {.colorWriteMask = VK_COLOR_COMPONENT_R_BIT};
    const VkPipelineColorBlendStateCreateInfo blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend};
    const VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                               .stageCount = 2,
                                               .pStages = state.stages,
                                               .pVertexInputState = &input,
                                               .pInputAssemblyState = &state.assembly,
                                               .pViewportState = &state.viewport,
                                               .pRasterizationState = &raster,
                                               .pMultisampleState = &state.samples,
                                               .pDepthStencilState = &depthState,
                                               .pColorBlendState = &blending,
                                               .pDynamicState = &state.dynamic,
                                               .layout = pipelineLayout,
                                               .renderPass = halo.maskPass};
    return create(device, info, out);
  }

  bool RasterRenderer::Gpu::prepareCoverage() {
    auto &h = halo;
    if (h.coverageReady)
      return true;
    if (!pass || h.failed)
      return false;
    if (!h.sampler) {
      const VkSamplerCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                        .magFilter = VK_FILTER_LINEAR,
                                        .minFilter = VK_FILTER_LINEAR,
                                        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                        .maxAnisotropy = 1};
      check(pipeline::createSampler(device, &info, nullptr, &h.sampler));
    }
    if (!h.maskFragment)
      h.maskFragment = module(device, haloMaskShader, sizeof(haloMaskShader));

    const auto attachmentStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const auto attachmentAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    // The targets were last sampled (an earlier group or frame); the depth
    // attachment was last written by the target pass. Afterwards the targets
    // are sampled, and the target pass resumes on the depth attachment.
    // Mask targets that cut traced instances are also sampled by the path
    // tracer's compute passes.
    const auto samplingStages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    const VkSubpassDependency dependencies[] = {
        {.srcSubpass = VK_SUBPASS_EXTERNAL,
         .dstSubpass = 0,
         .srcStageMask = attachmentStages | samplingStages,
         .dstStageMask = attachmentStages,
         .srcAccessMask = attachmentAccess | VK_ACCESS_SHADER_READ_BIT,
         .dstAccessMask = attachmentAccess},
        {.srcSubpass = 0,
         .dstSubpass = VK_SUBPASS_EXTERNAL,
         .srcStageMask = attachmentStages,
         .dstStageMask = attachmentStages | samplingStages,
         .srcAccessMask = attachmentAccess,
         .dstAccessMask = attachmentAccess | VK_ACCESS_SHADER_READ_BIT}};
    const VkAttachmentDescription attachments[] = {
        {.format = coverageFormat,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        // The frame's depth: tested, never written.
        {.format = depthFormat,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
         .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    const VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depthReference = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    for (int variant = 0; variant < 4; ++variant) {
      // loadPass keeps the target's contents (mask sources drawn in several runs).
      const bool keep = variant & 1, early = variant & 2;
      auto &out = early ? (keep ? h.earlyLoadPass : h.earlyPass) : keep ? h.loadPass : h.maskPass;
      if (out)
        continue;
      auto described = attachments[0];
      if (keep) {
        described.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        described.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      }
      // Before the target pass the depth attachment holds nothing (that pass
      // clears it): compatible with the silhouette pipelines, never tested.
      auto frameDepth = attachments[1];
      if (early) {
        frameDepth.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        frameDepth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        frameDepth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      }
      const VkAttachmentDescription both[] = {described, frameDepth};
      const VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            .colorAttachmentCount = 1,
                                            .pColorAttachments = &color,
                                            .pDepthStencilAttachment = &depthReference};
      const VkRenderPassCreateInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                           .attachmentCount = 2,
                                           .pAttachments = both,
                                           .subpassCount = 1,
                                           .pSubpasses = &subpass,
                                           .dependencyCount = 2,
                                           .pDependencies = dependencies};
      check(pipeline::createRenderPass(device, &info, nullptr, &out));
    }
    if (!h.blurPass) {
      // Every pixel of the render area is written.
      auto target = attachments[0];
      target.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      const VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            .colorAttachmentCount = 1,
                                            .pColorAttachments = &color};
      const VkRenderPassCreateInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                           .attachmentCount = 1,
                                           .pAttachments = &target,
                                           .subpassCount = 1,
                                           .pSubpasses = &subpass,
                                           .dependencyCount = 2,
                                           .pDependencies = dependencies};
      check(pipeline::createRenderPass(device, &info, nullptr, &h.blurPass));
    }

    bool ready = true;
    for (int tested = 0; tested < 2; ++tested)
      ready &= coveragePipeline(h.mask[tested], h.maskFragment, layout, tested != 0);
    h.coverageReady = ready;
    return ready;
  }

  bool RasterRenderer::Gpu::prepareHalo() {
    auto &h = halo;
    if (h.ready)
      return true;
    if (!pass || h.failed)
      return false;
    // Until every pipeline is ready, frames simply have no halos.
    bool ready = prepareCoverage();
    if (!h.layout) {
      // Set 0: the pass's source; set 1 (composite): the silhouette mask.
      const VkDescriptorSetLayout sets[] = {setLayout, setLayout};
      const VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BlurPush)};
      const VkPipelineLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                               .setLayoutCount = 2,
                                               .pSetLayouts = sets,
                                               .pushConstantRangeCount = 1,
                                               .pPushConstantRanges = &push};
      check(pipeline::createPipelineLayout(device, &info, nullptr, &h.layout));
    }
    if (!h.vertex)
      h.vertex = module(device, haloVertShader, sizeof(haloVertShader));
    if (!h.blurFragment)
      h.blurFragment = module(device, haloBlurShader, sizeof(haloBlurShader));
    if (!h.compositeFragment)
      h.compositeFragment = module(device, haloCompositeShader, sizeof(haloCompositeShader));
    PipelineState state;

    // Fullscreen passes: blur, and the composite onto the frame (additive, or
    // the reference's "Modulate": the frame times the blurred mask).
    const VkPipelineVertexInputStateCreateInfo noInput = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineRasterizationStateCreateInfo fullscreen = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1};
    const VkPipelineDepthStencilStateCreateInfo noDepth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    state.stages[0].module = h.vertex;
    for (int variant = 0; variant < 3; ++variant) {
      // 0: blur; 1: additive composite; 2: multiplicative composite.
      state.stages[1].module = variant ? h.compositeFragment : h.blurFragment;
      VkPipelineColorBlendAttachmentState blend = {.colorWriteMask = VK_COLOR_COMPONENT_R_BIT};
      if (variant) {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = variant == 1 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_DST_COLOR;
        blend.dstColorBlendFactor = variant == 1 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
      }
      const VkPipelineColorBlendStateCreateInfo blending = {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
          .attachmentCount = 1,
          .pAttachments = &blend};
      const VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                                 .stageCount = 2,
                                                 .pStages = state.stages,
                                                 .pVertexInputState = &noInput,
                                                 .pInputAssemblyState = &state.assembly,
                                                 .pViewportState = &state.viewport,
                                                 .pRasterizationState = &fullscreen,
                                                 .pMultisampleState = &state.samples,
                                                 .pDepthStencilState = &noDepth,
                                                 .pColorBlendState = &blending,
                                                 .pDynamicState = &state.dynamic,
                                                 .layout = h.layout,
                                                 .renderPass = variant ? pass : h.blurPass};
      ready &= create(device, info, variant == 0 ? h.blur : h.composite[variant == 1]);
    }
    h.ready = ready;
    return ready;
  }

  void RasterRenderer::Gpu::createCoverageTarget(Halo::Target &target, bool tested) {
    const VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                    .imageType = VK_IMAGE_TYPE_2D,
                                    .format = coverageFormat,
                                    .extent = {extent.width, extent.height, 1},
                                    .mipLevels = 1,
                                    .arrayLayers = 1,
                                    .samples = VK_SAMPLE_COUNT_1_BIT,
                                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                                    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    check(vkCreateImage(device, &info, nullptr, &target.image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, target.image, &requirements);
    const VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    check(vkAllocateMemory(device, &allocation, nullptr, &target.memory));
    check(vkBindImageMemory(device, target.image, target.memory, 0));
    const VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                        .image = target.image,
                                        .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                        .format = coverageFormat,
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    check(vkCreateImageView(device, &view, nullptr, &target.view));
    // A silhouette target tests the frame's depth attachment.
    const VkImageView views[] = {target.view, depth->view};
    const VkFramebufferCreateInfo framebuffer = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                                 .renderPass = tested ? halo.maskPass : halo.blurPass,
                                                 .attachmentCount = tested ? 2u : 1u,
                                                 .pAttachments = views,
                                                 .width = extent.width,
                                                 .height = extent.height,
                                                 .layers = 1};
    check(vkCreateFramebuffer(device, &framebuffer, nullptr, &target.framebuffer));
    const VkDescriptorSetAllocateInfo sets = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                              .descriptorPool = pool,
                                              .descriptorSetCount = 1,
                                              .pSetLayouts = &setLayout};
    check(vkAllocateDescriptorSets(device, &sets, &target.set));
    const VkDescriptorImageInfo image = {halo.sampler, target.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                        .dstSet = target.set,
                                        .dstBinding = 0,
                                        .descriptorCount = 1,
                                        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                        .pImageInfo = &image};
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  }

  void RasterRenderer::Gpu::createHaloTargets() {
    auto &h = halo;
    if (h.targets[0].image)
      return;
    for (auto &target : h.targets)
      createCoverageTarget(target, &target == &h.targets[0]);
  }

  bool RasterRenderer::Gpu::coverageArea(Halo::Group &group, const float *margin) const {
    if (!group.count || !group.bounded || !extent.width || !extent.height)
      return false;
    const float size[2] = {float(extent.width), float(extent.height)};
    int32_t low[2], high[2];
    for (int c = 0; c < 2; ++c) {
      const float a = (group.low[c] * .5f + .5f) * size[c] - margin[c];
      const float b = (group.high[c] * .5f + .5f) * size[c] + margin[c];
      low[c] = int32_t(std::floor(std::clamp(a, 0.f, size[c])));
      high[c] = int32_t(std::ceil(std::clamp(b, 0.f, size[c])));
      if (high[c] <= low[c])
        return false;
    }
    group.area = {{low[0], low[1]}, {uint32_t(high[0] - low[0]), uint32_t(high[1] - low[1])}};
    return true;
  }

  bool RasterRenderer::Gpu::haloArea(Halo::Group &group) const {
    const float blur[2] = {group.halo.blurX, group.halo.blurY};
    float margin[2];
    // Each iteration spreads the silhouette by the kernel's reach.
    for (int c = 0; c < 2; ++c)
      margin[c] = blurIterations * kernel(blur[c], extent.height).reach + 2;
    return coverageArea(group, margin);
  }

  uint32_t RasterRenderer::haloCount() const { return m_gpu ? uint32_t(m_gpu->halo.groups.size()) : 0; }

  void RasterRenderer::recordHaloMask(VkCommandBuffer command, uint32_t index) {
    auto &g = *m_gpu;
    auto &h = g.halo;
    const auto &group = h.groups[index];
    const VkViewport viewport = {0, 0, float(g.extent.width), float(g.extent.height), 0, 1};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &group.area);
    VkClearValue clear = {};
    VkRenderPassBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                   .renderPass = h.maskPass,
                                   .framebuffer = h.targets[0].framebuffer,
                                   .renderArea = group.area,
                                   .clearValueCount = 1,
                                   .pClearValues = &clear};
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    for (uint32_t i = group.first; i < group.first + group.count; ++i) {
      const auto &draw = h.commands[i];
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &draw.set, 0, nullptr);
      vkCmdBindVertexBuffers(command, 0, 1, &draw.vertices, &draw.vertexOffset);
      vkCmdBindIndexBuffer(command, draw.indices, draw.indexOffset, VK_INDEX_TYPE_UINT32);
      vkCmdPushConstants(command, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(Push), &draw.push);
      vkCmdDrawIndexed(command, draw.count, 1, draw.firstIndex, draw.baseVertex, 0);
    }
    vkCmdEndRenderPass(command);

    // Horizontal then vertical, twice: mask -> A -> B -> A -> B.
    const Kernel kernels[2] = {kernel(group.halo.blurX, g.extent.height),
                               kernel(group.halo.blurY, g.extent.height)};
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, h.blur);
    begin.renderPass = h.blurPass;
    begin.clearValueCount = 0;
    int source = 0;
    for (int pass = 0; pass < 2 * blurIterations; ++pass) {
      const int axis = pass & 1;
      const int target = axis ? 2 : 1;
      const auto &k = kernels[axis];
      BlurPush push = {};
      push.area[0] = float(group.area.offset.x) + .5f;
      push.area[1] = float(group.area.offset.y) + .5f;
      push.area[2] = float(group.area.offset.x + int32_t(group.area.extent.width)) - .5f;
      push.area[3] = float(group.area.offset.y + int32_t(group.area.extent.height)) - .5f;
      push.texel[0] = 1.f / float(g.extent.width);
      push.texel[1] = 1.f / float(g.extent.height);
      push.direction[axis] = 1;
      push.sigma = k.sigma;
      push.stride = k.stride;
      push.taps = k.taps;
      begin.framebuffer = h.targets[target].framebuffer;
      vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, h.layout, 0, 1, &h.targets[source].set, 0,
                              nullptr);
      vkCmdPushConstants(command, h.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
      vkCmdDraw(command, 3, 1, 0, 0);
      vkCmdEndRenderPass(command);
      source = target;
    }
  }

  void RasterRenderer::recordHaloComposite(VkCommandBuffer command, uint32_t index) {
    auto &g = *m_gpu;
    auto &h = g.halo;
    const auto &group = h.groups[index];
    const VkViewport viewport = {0, 0, float(g.extent.width), float(g.extent.height), 0, 1};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &group.area);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, h.composite[group.halo.additive]);
    // The blurred coverage (the last blur target) and the silhouette mask.
    const VkDescriptorSet sets[] = {h.targets[2].set, h.targets[0].set};
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, h.layout, 0, 2, sets, 0, nullptr);
    CompositePush push = {};
    std::copy_n(group.halo.color, 3, push.color);
    push.color[3] = float(group.halo.passes);
    push.flags = uint32_t(group.halo.additive);
    vkCmdPushConstants(command, h.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(command, 3, 1, 0, 0);
  }
} // namespace remix
