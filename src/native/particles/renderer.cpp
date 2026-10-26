#include "particles/renderer.h"
#include "particles/shaders/embedded.h"
#include "platform/gpu_frames.h"
#include "platform/gpu_timers.h"
#include "runtime/pipelines.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace remix::particles {
  namespace {
    void check(VkResult result) {
      if (result != VK_SUCCESS)
        throw result;
    }

    // particles/shaders/common.glsl, std430.
    struct GpuFrame {
      float viewProjection[16];
      float camera[4], cameraRight[4], cameraUp[4], cameraDelta[4], worldUp[4];
      float sunDirection[4], sunRadiance[4], ambient[4], volumeOrigin[4];
      int32_t volumeCell[4];
      uint32_t volumeSize[4], display[4];
      float depthScale[4];
      float dome[3][4];
      uint32_t counts[4];
      float cloudShadow[3][4];
      uint32_t cloudShadowConfig[4];
    };
    static_assert(sizeof(GpuFrame) == 400);
    struct GpuEmitter {
      float transform[3][4];
      float extent[4];
      uint32_t kind[4], slice[4], groups[4];
      float velocity[4], axis[4], acceleration[4], motion[4], size[4], colorStart[4], colorEnd[4], emission[4];
    };
    static_assert(sizeof(GpuEmitter) == 240);
    constexpr uint32_t particleBytes = 64;
    constexpr uint32_t maxEmitters = scene::maxParticleEmitters;
    constexpr uint32_t group = 64;
    // The light volume: cells of a metre, half as many along the up axis.
    constexpr uint32_t volumeAcross = 32, volumeUp = 16;
    constexpr uint32_t volumeCells = volumeAcross * volumeAcross * volumeUp;
    constexpr VkDeviceSize frameBytes = sizeof(GpuFrame) + sizeof(GpuEmitter) * maxEmitters;
    constexpr VkDeviceSize lightBytes = (4 + volumeCells) * sizeof(float);

    struct Buffer {
      VkDevice device{};
      VkBuffer handle{};
      VkDeviceMemory memory{};
      void *mapped{};
      VkDeviceSize bytes{};
      ~Buffer() {
        if (mapped)
          vkUnmapMemory(device, memory);
        if (handle)
          vkDestroyBuffer(device, handle, nullptr);
        if (memory)
          vkFreeMemory(device, memory, nullptr);
      }
    };
    uint32_t memoryType(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags flags) {
      VkPhysicalDeviceMemoryProperties memory;
      vkGetPhysicalDeviceMemoryProperties(physical, &memory);
      for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags)
          return i;
      throw VK_ERROR_FEATURE_NOT_PRESENT;
    }
    std::unique_ptr<Buffer> buffer(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size,
                                   VkBufferUsageFlags usage, bool host = false) {
      auto out = std::make_unique<Buffer>();
      out->device = device;
      const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
      check(vkCreateBuffer(device, &info, nullptr, &out->handle));
      VkMemoryRequirements requirements;
      vkGetBufferMemoryRequirements(device, out->handle, &requirements);
      out->bytes = size;
      const VkMemoryAllocateInfo allocation{
          .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
          .allocationSize = requirements.size,
          .memoryTypeIndex = memoryType(physical, requirements.memoryTypeBits,
                                        host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                             : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
      check(vkAllocateMemory(device, &allocation, nullptr, &out->memory));
      check(vkBindBufferMemory(device, out->handle, out->memory, 0));
      if (host)
        check(vkMapMemory(device, out->memory, 0, VK_WHOLE_SIZE, 0, &out->mapped));
      return out;
    }
    void memoryBarrier(VkCommandBuffer command, VkPipelineStageFlags source, VkAccessFlags sourceAccess,
                       VkPipelineStageFlags target, VkAccessFlags targetAccess) {
      const VkMemoryBarrier barrier{
          .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = sourceAccess, .dstAccessMask = targetAccess};
      vkCmdPipelineBarrier(command, source, target, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    void computeBarrier(VkCommandBuffer command) {
      memoryBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    uint32_t groups(uint64_t threads) { return uint32_t((threads + group - 1) / group); }
  } // namespace

  struct Renderer::Gpu {
    VkPhysicalDevice physical;
    VkDevice device;
    bool rayQuery;
    // Compute stages: simulate, spawn, args, light (traced), spawn (traced).
    enum Stage { Simulate, Spawn, Args, Light, SpawnTraced, StageCount };
    struct Compute {
      VkShaderModule module = VK_NULL_HANDLE;
      VkPipeline pipeline = VK_NULL_HANDLE;
    } compute[StageCount];
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    VkDescriptorSetLayout poolLayout = VK_NULL_HANDLE, tracedLayout = VK_NULL_HANDLE, drawLayout = VK_NULL_HANDLE;
    VkPipelineLayout computeLayout = VK_NULL_HANDLE, tracedComputeLayout = VK_NULL_HANDLE,
                     drawPipelineLayout = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkPipeline draw[2] = {}; // additive, premultiplied
    VkSampler sampler = VK_NULL_HANDLE, nearest = VK_NULL_HANDLE;
    // A white texel: the texture of emitters without one, and the depth
    // binding when the scene's depth is not captured.
    VkImage white = VK_NULL_HANDLE;
    VkImageView whiteView = VK_NULL_HANDLE;
    VkDeviceMemory whiteMemory = VK_NULL_HANDLE;
    bool whiteReady = false;

    uint32_t budget = 0;
    std::unique_ptr<Buffer> particles[2], counts[2], indirect, statistics, light;
    bool lightReady = false, countsReady = false;
    uint32_t parity = 0; // the target pool this frame
    struct Slot {
      std::unique_ptr<Buffer> frame, readback;
      VkDescriptorPool pool = VK_NULL_HANDLE;
      bool submitted = false;
    } slots[gpuFrameCount];
    uint32_t slot = 0;

    // The frame being drawn.
    VkDescriptorSet poolSet = VK_NULL_HANDLE;
    std::vector<uint32_t> blends;
    std::vector<VkDescriptorImageInfo> textures; // per emitter; null view: the round sprite
    uint32_t drawn = 0;
    bool depthBound = false;
    // Resets asked for while the passes could not run.
    std::array<bool, maxEmitters> pendingReset{};
    bool pendingResetAll = true;
    float previousCamera[3] = {};
    bool cameraKnown = false;

    ~Gpu();
    void createLayouts();
    bool preparePipelines(bool traced, VkRenderPass target);
    void ensurePool(VkCommandBuffer command, uint32_t size);
    void ensureWhite(VkCommandBuffer command);
  };

  Renderer::Gpu::~Gpu() {
    for (auto &stage : compute) {
      if (stage.pipeline)
        pipeline::destroyPipeline(device, stage.pipeline, nullptr);
      if (stage.module)
        pipeline::destroyShaderModule(device, stage.module, nullptr);
    }
    for (auto drawPipeline : draw)
      if (drawPipeline)
        pipeline::destroyPipeline(device, drawPipeline, nullptr);
    for (auto module : {vertex, fragment})
      if (module)
        pipeline::destroyShaderModule(device, module, nullptr);
    for (auto layout : {computeLayout, tracedComputeLayout, drawPipelineLayout})
      if (layout)
        pipeline::destroyPipelineLayout(device, layout, nullptr);
    for (auto layout : {poolLayout, tracedLayout, drawLayout})
      if (layout)
        pipeline::destroyDescriptorSetLayout(device, layout, nullptr);
    for (auto s : {sampler, nearest})
      if (s)
        pipeline::destroySampler(device, s, nullptr);
    for (auto &s : slots)
      if (s.pool)
        vkDestroyDescriptorPool(device, s.pool, nullptr);
    if (whiteView)
      vkDestroyImageView(device, whiteView, nullptr);
    if (white)
      vkDestroyImage(device, white, nullptr);
    if (whiteMemory)
      vkFreeMemory(device, whiteMemory, nullptr);
  }

  void Renderer::Gpu::createLayouts() {
    VkDescriptorSetLayoutBinding pool[8];
    for (uint32_t i = 0; i < 8; ++i)
      pool[i] = {.binding = i,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 .descriptorCount = 1,
                 .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 8, .pBindings = pool};
    check(pipeline::createDescriptorSetLayout(device, &info, nullptr, &poolLayout));
    const VkDescriptorSetLayoutBinding draws[] = {
        {.binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}};
    info.bindingCount = 2;
    info.pBindings = draws;
    check(pipeline::createDescriptorSetLayout(device, &info, nullptr, &drawLayout));
    const VkPushConstantRange computePush{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1,
                                      .pSetLayouts = &poolLayout,
                                      .pushConstantRangeCount = 1,
                                      .pPushConstantRanges = &computePush};
    check(pipeline::createPipelineLayout(device, &layout, nullptr, &computeLayout));
    const VkDescriptorSetLayout drawSets[] = {poolLayout, drawLayout};
    const VkPushConstantRange drawPush{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    layout.setLayoutCount = 2;
    layout.pSetLayouts = drawSets;
    layout.pPushConstantRanges = &drawPush;
    check(pipeline::createPipelineLayout(device, &layout, nullptr, &drawPipelineLayout));
    if (rayQuery) {
      const VkDescriptorSetLayoutBinding traced[] = {
          {.binding = 0,
           .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
           .descriptorCount = 1,
           .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
          {.binding = 1,
           .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
           .descriptorCount = 1,
           .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
          {.binding = 2,
           .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
           .descriptorCount = 1,
           .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
      info.bindingCount = 3;
      info.pBindings = traced;
      check(pipeline::createDescriptorSetLayout(device, &info, nullptr, &tracedLayout));
      const VkDescriptorSetLayout tracedSets[] = {poolLayout, tracedLayout};
      layout.setLayoutCount = 2;
      layout.pSetLayouts = tracedSets;
      layout.pPushConstantRanges = &computePush;
      check(pipeline::createPipelineLayout(device, &layout, nullptr, &tracedComputeLayout));
    }
    VkSamplerCreateInfo samplerInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                    .magFilter = VK_FILTER_LINEAR,
                                    .minFilter = VK_FILTER_LINEAR,
                                    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                    .maxLod = VK_LOD_CLAMP_NONE};
    check(pipeline::createSampler(device, &samplerInfo, nullptr, &sampler));
    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    check(pipeline::createSampler(device, &samplerInfo, nullptr, &nearest));
  }

  bool Renderer::Gpu::preparePipelines(bool traced, VkRenderPass target) {
    struct Code {
      const uint32_t *words;
      size_t bytes;
    };
    const Code codes[StageCount] = {{simulateShader, sizeof(simulateShader)},
                                    {spawnShader, sizeof(spawnShader)},
                                    {argsShader, sizeof(argsShader)},
                                    {lightShader, sizeof(lightShader)},
                                    {spawnTracedShader, sizeof(spawnTracedShader)}};
    const auto module = [&](VkShaderModule &out, const uint32_t *words, size_t bytes) {
      if (out)
        return;
      const VkShaderModuleCreateInfo info{
          .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = words};
      check(pipeline::createShaderModule(device, &info, nullptr, &out));
    };
    bool ready = true;
    for (int stage = 0; stage < StageCount; ++stage) {
      const bool tracedStage = stage == Light || stage == SpawnTraced;
      if (tracedStage && !(traced && rayQuery))
        continue;
      auto &c = compute[stage];
      if (c.pipeline)
        continue;
      module(c.module, codes[stage].words, codes[stage].bytes);
      const VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                             .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                       .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                       .module = c.module,
                                                       .pName = "main"},
                                             .layout = tracedStage ? tracedComputeLayout : computeLayout};
      const auto result = pipeline::createComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &c.pipeline);
      if (result != VK_SUCCESS && result != VK_NOT_READY)
        throw result;
      if (c.pipeline) {
        pipeline::destroyShaderModule(device, c.module, nullptr);
        c.module = VK_NULL_HANDLE;
      }
      // The traced stages are optional: without them the frame runs untraced.
      ready &= tracedStage || c.pipeline != VK_NULL_HANDLE;
    }
    if (target != pass) {
      for (auto &drawPipeline : draw)
        if (drawPipeline) {
          pipeline::destroyPipeline(device, drawPipeline, nullptr);
          drawPipeline = VK_NULL_HANDLE;
        }
      pass = target;
    }
    module(vertex, vertShader, sizeof(vertShader));
    module(fragment, fragShader, sizeof(fragShader));
    for (uint32_t blend = 0; blend < 2; ++blend) {
      if (draw[blend])
        continue;
      const VkPipelineShaderStageCreateInfo stages[] = {
          {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
           .stage = VK_SHADER_STAGE_VERTEX_BIT,
           .module = vertex,
           .pName = "main"},
          {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
           .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
           .module = fragment,
           .pName = "main"}};
      const VkPipelineVertexInputStateCreateInfo input{.sType =
                                                            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      const VkPipelineInputAssemblyStateCreateInfo assembly{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
          .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
      const VkPipelineViewportStateCreateInfo viewport{.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                       .viewportCount = 1,
                                                       .scissorCount = 1};
      const VkPipelineRasterizationStateCreateInfo raster{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
          .polygonMode = VK_POLYGON_MODE_FILL,
          .cullMode = VK_CULL_MODE_NONE,
          .lineWidth = 1};
      const VkPipelineMultisampleStateCreateInfo multisample{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
          .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
      const VkPipelineDepthStencilStateCreateInfo depth{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
          .depthTestEnable = VK_TRUE,
          .depthWriteEnable = VK_FALSE,
          .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
      // The fragment shader's colour is premultiplied by its alpha.
      const VkPipelineColorBlendAttachmentState attachment{
          .blendEnable = VK_TRUE,
          .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
          .dstColorBlendFactor = blend ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE,
          .colorBlendOp = VK_BLEND_OP_ADD,
          .srcAlphaBlendFactor = blend ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO,
          .dstAlphaBlendFactor = blend ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE,
          .alphaBlendOp = VK_BLEND_OP_ADD,
          .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                            VK_COLOR_COMPONENT_A_BIT};
      const VkPipelineColorBlendStateCreateInfo blending{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
          .attachmentCount = 1,
          .pAttachments = &attachment};
      const VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
      const VkPipelineDynamicStateCreateInfo dynamic{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                     .dynamicStateCount = 2,
                                                     .pDynamicStates = dynamics};
      const VkGraphicsPipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                              .stageCount = 2,
                                              .pStages = stages,
                                              .pVertexInputState = &input,
                                              .pInputAssemblyState = &assembly,
                                              .pViewportState = &viewport,
                                              .pRasterizationState = &raster,
                                              .pMultisampleState = &multisample,
                                              .pDepthStencilState = &depth,
                                              .pColorBlendState = &blending,
                                              .pDynamicState = &dynamic,
                                              .layout = drawPipelineLayout,
                                              .renderPass = pass};
      const auto result = pipeline::createGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &draw[blend]);
      if (result != VK_SUCCESS && result != VK_NOT_READY)
        throw result;
      ready &= draw[blend] != VK_NULL_HANDLE;
    }
    return ready;
  }

  void Renderer::Gpu::ensurePool(VkCommandBuffer command, uint32_t size) {
    if (!indirect) {
      indirect = buffer(physical, device, sizeof(VkDrawIndirectCommand) * maxEmitters,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
      statistics = buffer(physical, device, 16,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT);
      light = buffer(physical, device, lightBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
      for (auto &c : counts)
        c = buffer(physical, device, sizeof(uint32_t) * maxEmitters,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
      for (auto &s : slots) {
        s.frame = buffer(physical, device, frameBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        s.readback = buffer(physical, device, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        std::memset(s.readback->mapped, 0, 16);
        const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8},
                                              {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * (maxEmitters + 3)},
                                              {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1}};
        const VkDescriptorPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .maxSets = maxEmitters + 2,
                                              .poolSizeCount = rayQuery ? 3u : 2u,
                                              .pPoolSizes = sizes};
        check(vkCreateDescriptorPool(device, &info, nullptr, &s.pool));
      }
    }
    if (size != budget || !particles[0]) {
      for (auto &p : particles)
        p = buffer(physical, device, VkDeviceSize(std::max(size, 1u)) * particleBytes,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      budget = size;
      countsReady = false;
    }
    if (!countsReady || !lightReady) {
      // Empty slices; every cell sees the sun until traced.
      for (auto &c : counts)
        vkCmdFillBuffer(command, c->handle, 0, VK_WHOLE_SIZE, 0);
      if (!lightReady)
        vkCmdFillBuffer(command, light->handle, 0, VK_WHOLE_SIZE, 0x3f800000u);
      countsReady = lightReady = true;
      memoryBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
  }

  void Renderer::Gpu::ensureWhite(VkCommandBuffer command) {
    if (whiteReady)
      return;
    const VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                 .imageType = VK_IMAGE_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM,
                                 .extent = {1, 1, 1},
                                 .mipLevels = 1,
                                 .arrayLayers = 1,
                                 .samples = VK_SAMPLE_COUNT_1_BIT,
                                 .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    check(vkCreateImage(device, &info, nullptr, &white));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, white, &requirements);
    const VkMemoryAllocateInfo allocation{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memoryType(physical, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    check(vkAllocateMemory(device, &allocation, nullptr, &whiteMemory));
    check(vkBindImageMemory(device, white, whiteMemory, 0));
    const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = white,
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = VK_FORMAT_R8G8B8A8_UNORM,
                                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    check(vkCreateImageView(device, &view, nullptr, &whiteView));
    VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                 .srcAccessMask = 0,
                                 .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                 .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                 .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .image = white,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    const VkClearColorValue one{.float32 = {1, 1, 1, 1}};
    vkCmdClearColorImage(command, white, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &barrier.subresourceRange);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
    whiteReady = true;
  }

  Renderer::Renderer(VkPhysicalDevice physical, VkDevice device, bool rayQuery) : m(std::make_unique<Gpu>()) {
    m->physical = physical;
    m->device = device;
    m->rayQuery = rayQuery;
    m->createLayouts();
  }

  Renderer::~Renderer() = default;

  void Renderer::selectFrame(uint32_t slot) {
    auto &g = *m;
    g.slot = slot % gpuFrameCount;
    auto &s = g.slots[g.slot];
    if (s.pool)
      vkResetDescriptorPool(g.device, s.pool, 0);
    if (s.submitted && s.readback) {
      const auto *values = static_cast<const uint32_t *>(s.readback->mapped);
      m_report.live = values[0];
      m_report.dropped = values[1];
      m_report.sheltered = values[2];
      s.submitted = false;
    }
  }

  void Renderer::defer(const scene::ParticleFrame &frame) {
    for (const auto &emitter : frame.emitters)
      if (emitter.reset)
        m->pendingReset[emitter.slot] = true;
    m->pendingResetAll |= frame.resetAll;
    m->drawn = 0;
  }

  bool Renderer::simulate(VkCommandBuffer command, const scene::ParticleFrame &frame, const FrameInputs &inputs,
                          const TextureLookup &texture, VkRenderPass target) {
    auto &g = *m;
    g.drawn = 0;
    if (!frame.enabled || !frame.budget) {
      g.pendingResetAll = true;
      g.cameraKnown = false;
      return false;
    }
    for (const auto &emitter : frame.emitters)
      if (emitter.reset)
        g.pendingReset[emitter.slot] = true;
    g.pendingResetAll |= frame.resetAll;
    const bool traced = inputs.scene != VK_NULL_HANDLE && g.rayQuery;
    if (!g.preparePipelines(traced, target))
      return false;
    const bool tracedReady = traced && g.compute[Gpu::Light].pipeline && g.compute[Gpu::SpawnTraced].pipeline;
    gpu::GpuScope scope(command, "Particles: simulation");
    g.ensurePool(command, frame.budget);
    if (g.pendingResetAll) {
      for (auto &c : g.counts)
        vkCmdFillBuffer(command, c->handle, 0, VK_WHOLE_SIZE, 0);
      memoryBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
      g.pendingResetAll = false;
    }
    g.ensureWhite(command);
    auto &slot = g.slots[g.slot];

    // The frame's constants.
    auto *out = static_cast<GpuFrame *>(slot.frame->mapped);
    GpuFrame f{};
    std::memcpy(f.viewProjection, inputs.viewProjection.data(), sizeof(f.viewProjection));
    const auto &v = inputs.view;
    float camera[3];
    for (int c = 0; c < 3; ++c)
      camera[c] = -(v[c * 4 + 0] * v[12] + v[c * 4 + 1] * v[13] + v[c * 4 + 2] * v[14]);
    const float seconds = frame.seconds;
    for (int i = 0; i < 3; ++i) {
      f.camera[i] = camera[i];
      f.cameraRight[i] = v[i * 4 + 0];
      f.cameraUp[i] = v[i * 4 + 1];
      f.cameraDelta[i] = g.cameraKnown ? camera[i] - g.previousCamera[i] : 0;
      g.previousCamera[i] = camera[i];
      f.sunDirection[i] = inputs.sunDirection[i];
      f.sunRadiance[i] = inputs.sunIrradiance[i];
      f.ambient[i] = inputs.ambient[i];
    }
    g.cameraKnown = true;
    f.camera[3] = seconds;
    f.cameraRight[3] = inputs.time;
    const uint32_t up = std::min(inputs.upAxis, 2u);
    f.worldUp[up] = 1;
    f.worldUp[3] = inputs.exposure;
    // The light volume: a metre a cell around the camera, snapped to cells.
    const float cell = 1 / std::max(inputs.metersPerUnit, 1e-6f);
    for (int i = 0; i < 3; ++i) {
      const uint32_t size = uint32_t(i) == up ? volumeUp : volumeAcross;
      f.volumeSize[i] = size;
      f.volumeCell[i] = int32_t(std::floor(camera[i] / cell)) - int32_t(size / 2) + (uint32_t(i) == up ? 2 : 0);
      f.volumeOrigin[i] = float(f.volumeCell[i]) * cell;
    }
    f.volumeOrigin[3] = cell;
    f.volumeCell[3] = int32_t(frame.frame % 4);
    f.sunRadiance[3] = tracedReady ? 1.f : 0.f;
    const bool environment = tracedReady && inputs.environment.imageView != VK_NULL_HANDLE;
    f.ambient[3] = environment ? 1.f : 0.f;
    std::copy_n(inputs.display, 4, f.display);
    // View depth from the device depth, as raster.vert's sprites.
    const auto &p = inputs.viewProjection;
    const float zRow[3] = {p[2], p[6], p[10]}, wRow[3] = {p[3], p[7], p[11]};
    const float ww = wRow[0] * wRow[0] + wRow[1] * wRow[1] + wRow[2] * wRow[2];
    if (ww > 1e-20f) {
      const float a = (zRow[0] * wRow[0] + zRow[1] * wRow[1] + zRow[2] * wRow[2]) / ww;
      f.depthScale[0] = a;
      f.depthScale[1] = p[14] - a * p[15];
      f.depthScale[2] = 1;
    }
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c)
        f.dome[r][c] = inputs.domeTransform[r][c];
    f.dome[0][3] = inputs.environmentScale;
    const bool clouds = tracedReady && inputs.cloudShadow.imageView != VK_NULL_HANDLE && inputs.cloudShadowSlices >= 2;
    if (clouds) {
      std::memcpy(f.cloudShadow, inputs.cloudShadowTransform, sizeof(f.cloudShadow));
      f.cloudShadowConfig[0] = 1;
      f.cloudShadowConfig[1] = inputs.cloudShadowSlices;
    }
    const uint32_t emitterCount = uint32_t(std::min<size_t>(frame.emitters.size(), maxEmitters));
    uint32_t simulateGroups = 0, spawns = 0;
    g.blends.assign(emitterCount, 0);
    g.textures.assign(emitterCount, VkDescriptorImageInfo{});
    auto *emitters = reinterpret_cast<GpuEmitter *>(reinterpret_cast<char *>(out) + sizeof(GpuFrame));
    for (uint32_t i = 0; i < emitterCount; ++i) {
      const auto &e = frame.emitters[i];
      GpuEmitter x{};
      std::memcpy(x.transform, e.transform, sizeof(x.transform));
      std::copy_n(e.extent, 3, x.extent);
      x.kind[0] = e.shape;
      x.kind[1] = e.facing;
      x.kind[2] = e.blend;
      // SHELTER needs the traced spawn pass.
      x.kind[3] = e.flags;
      x.slice[0] = e.offset;
      x.slice[1] = e.capacity;
      x.slice[2] = e.slot;
      x.slice[3] = e.seed * 2654435761u + e.slot;
      x.groups[0] = simulateGroups;
      x.groups[1] = spawns;
      x.groups[2] = e.spawn;
      x.groups[3] = g.pendingReset[e.slot] ? 1u : 0u;
      g.pendingReset[e.slot] = false;
      simulateGroups += groups(e.capacity);
      spawns += e.spawn;
      std::copy_n(e.velocity, 3, x.velocity);
      x.velocity[3] = e.speed;
      std::copy_n(e.axis, 3, x.axis);
      x.axis[3] = e.cosCone;
      std::copy_n(e.acceleration, 3, x.acceleration);
      x.acceleration[3] = e.drag;
      x.motion[0] = e.lifetime[0];
      x.motion[1] = e.lifetime[1];
      x.motion[2] = e.turbulence;
      x.motion[3] = e.stretch;
      std::copy_n(e.size, 3, x.size);
      x.size[3] = e.softDistance;
      std::copy_n(e.colorStart, 4, x.colorStart);
      std::copy_n(e.colorEnd, 4, x.colorEnd);
      std::copy_n(e.emission, 3, x.emission);
      if (e.texture && texture) {
        g.textures[i] = texture(e.texture, command);
        x.emission[3] = g.textures[i].imageView ? 1.f : 0.f;
      }
      g.blends[i] = e.blend;
      emitters[i] = x;
    }
    f.volumeSize[3] = emitterCount;
    f.counts[0] = uint32_t(frame.frame);
    f.counts[1] = emitterCount;
    f.counts[2] = groups(spawns);
    f.counts[3] = simulateGroups;
    std::memcpy(out, &f, sizeof(f));

    // Descriptors: the pool's buffers, this frame's target the other one.
    const uint32_t targetPool = g.parity, sourcePool = g.parity ^ 1u;
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .descriptorPool = slot.pool,
                                               .descriptorSetCount = 1,
                                               .pSetLayouts = &g.poolLayout};
    check(vkAllocateDescriptorSets(g.device, &allocate, &g.poolSet));
    const VkDescriptorBufferInfo buffers[8] = {{slot.frame->handle, 0, VK_WHOLE_SIZE},
                                               {g.particles[sourcePool]->handle, 0, VK_WHOLE_SIZE},
                                               {g.particles[targetPool]->handle, 0, VK_WHOLE_SIZE},
                                               {g.counts[sourcePool]->handle, 0, VK_WHOLE_SIZE},
                                               {g.counts[targetPool]->handle, 0, VK_WHOLE_SIZE},
                                               {g.indirect->handle, 0, VK_WHOLE_SIZE},
                                               {g.statistics->handle, 0, VK_WHOLE_SIZE},
                                               {g.light->handle, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[8];
    for (uint32_t i = 0; i < 8; ++i)
      writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                   .dstSet = g.poolSet,
                   .dstBinding = i,
                   .descriptorCount = 1,
                   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                   .pBufferInfo = &buffers[i]};
    vkUpdateDescriptorSets(g.device, 8, writes, 0, nullptr);
    VkDescriptorSet tracedSet = VK_NULL_HANDLE;
    if (tracedReady) {
      const VkDescriptorSetAllocateInfo tracedAllocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                       .descriptorPool = slot.pool,
                                                       .descriptorSetCount = 1,
                                                       .pSetLayouts = &g.tracedLayout};
      check(vkAllocateDescriptorSets(g.device, &tracedAllocate, &tracedSet));
      const VkWriteDescriptorSetAccelerationStructureKHR accel{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
          .accelerationStructureCount = 1,
          .pAccelerationStructures = &inputs.scene};
      const VkDescriptorImageInfo white{g.sampler, g.whiteView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      const VkDescriptorImageInfo image = environment ? inputs.environment : white;
      const VkDescriptorImageInfo shadow = clouds ? inputs.cloudShadow : white;
      const VkWriteDescriptorSet tracedWrites[] = {
          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
           .pNext = &accel,
           .dstSet = tracedSet,
           .dstBinding = 0,
           .descriptorCount = 1,
           .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR},
          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
           .dstSet = tracedSet,
           .dstBinding = 1,
           .descriptorCount = 1,
           .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
           .pImageInfo = &image},
          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
           .dstSet = tracedSet,
           .dstBinding = 2,
           .descriptorCount = 1,
           .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
           .pImageInfo = &shadow}};
      vkUpdateDescriptorSets(g.device, 3, tracedWrites, 0, nullptr);
    }

    // The passes.
    vkCmdFillBuffer(command, g.counts[targetPool]->handle, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(command, g.statistics->handle, 0, VK_WHOLE_SIZE, 0);
    memoryBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    const auto bind = [&](Gpu::Stage stage) {
      const bool tracedStage = stage == Gpu::Light || stage == Gpu::SpawnTraced;
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, g.compute[stage].pipeline);
      const VkDescriptorSet sets[] = {g.poolSet, tracedSet};
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                              tracedStage ? g.tracedComputeLayout : g.computeLayout, 0, tracedStage ? 2 : 1, sets, 0,
                              nullptr);
    };
    if (tracedReady) {
      // The TLAS build and the environment's writes precede these reads.
      memoryBarrier(command,
                    VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT);
      gpu::GpuScope lightScope(command, "Particles: light volume");
      bind(Gpu::Light);
      vkCmdDispatch(command, 1 + groups(volumeCells / 4), 1, 1);
      computeBarrier(command);
    }
    if (simulateGroups) {
      bind(Gpu::Simulate);
      vkCmdDispatch(command, simulateGroups, 1, 1);
      computeBarrier(command);
    }
    if (spawns) {
      bind(tracedReady ? Gpu::SpawnTraced : Gpu::Spawn);
      vkCmdDispatch(command, groups(spawns), 1, 1);
      computeBarrier(command);
    }
    bind(Gpu::Args);
    vkCmdDispatch(command, groups(emitterCount ? emitterCount : 1), 1, 1);
    memoryBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferCopy copy{0, 0, 16};
    vkCmdCopyBuffer(command, g.statistics->handle, slot.readback->handle, 1, &copy);
    slot.submitted = true;
    g.parity ^= 1u;
    g.drawn = emitterCount;
    return emitterCount != 0;
  }

  void Renderer::draw(VkCommandBuffer command, VkExtent2D extent, VkDescriptorImageInfo sceneDepth) {
    auto &g = *m;
    if (!g.drawn || !g.poolSet)
      return;
    gpu::GpuScope scope(command, "Particles: draw", extent);
    auto &slot = g.slots[g.slot];
    const bool depth = sceneDepth.imageView != VK_NULL_HANDLE;
    const VkDescriptorImageInfo white{g.sampler, g.whiteView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    auto *frame = static_cast<GpuFrame *>(slot.frame->mapped);
    // Soft edges need the captured depth: without it the shader keeps them hard.
    if (!depth)
      frame->depthScale[2] = 0;
    frame->dome[1][3] = float(extent.width);
    frame->dome[2][3] = float(extent.height);
    const VkViewport viewport{0, 0, float(extent.width), float(extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    // A draw set per distinct texture.
    std::unordered_map<VkImageView, VkDescriptorSet> sets;
    uint32_t bound = UINT32_MAX;
    for (uint32_t i = 0; i < g.drawn; ++i) {
      const auto &image = g.textures[i].imageView ? g.textures[i] : white;
      auto &set = sets[image.imageView];
      if (!set) {
        const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                   .descriptorPool = slot.pool,
                                                   .descriptorSetCount = 1,
                                                   .pSetLayouts = &g.drawLayout};
        if (vkAllocateDescriptorSets(g.device, &allocate, &set) != VK_SUCCESS)
          return;
        const VkDescriptorImageInfo depthImage =
            depth ? VkDescriptorImageInfo{g.nearest, sceneDepth.imageView, sceneDepth.imageLayout} : white;
        const VkWriteDescriptorSet writes[] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                .dstSet = set,
                                                .dstBinding = 0,
                                                .descriptorCount = 1,
                                                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                .pImageInfo = &depthImage},
                                               {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                .dstSet = set,
                                                .dstBinding = 1,
                                                .descriptorCount = 1,
                                                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                .pImageInfo = &image}};
        vkUpdateDescriptorSets(g.device, 2, writes, 0, nullptr);
      }
      const uint32_t blend = std::min(g.blends[i], 1u);
      if (blend != bound) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.draw[blend]);
        bound = blend;
      }
      const VkDescriptorSet descriptorSets[] = {g.poolSet, set};
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.drawPipelineLayout, 0, 2, descriptorSets, 0,
                              nullptr);
      const uint32_t push[4] = {i, 0, 0, 0};
      vkCmdPushConstants(command, g.drawPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(push), push);
      vkCmdDrawIndirect(command, g.indirect->handle, VkDeviceSize(i) * sizeof(VkDrawIndirectCommand), 1,
                        sizeof(VkDrawIndirectCommand));
    }
  }
} // namespace remix::particles
