#include "forward/cascades/cascades.h"
#include "forward/cascades/shaders/embedded.h"
#include "forward/options.h"
#include "forward/resources.h"
#include "platform/gpu_frames.h"
#include "platform/gpu_timers.h"
#include "runtime/pipelines.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

namespace remix::forward::cascades {
  namespace {
    constexpr uint32_t maximumCascades = 6;
    // World irradiance cache cells (a power of two): keys, samples and values
    // take 4 + 16 + 16 bytes each, 36 MiB in all.
    constexpr uint32_t cacheEntries = 1u << 20;
    constexpr float cacheCellMetres = .25f; // at the camera; cells double with distance
    struct CascadeFrame {
      Matrix inverseCamera, view, previousCamera, worldToDome;
      Vec4 eye{}, interval{}, control{}, skyRadiance{};
      std::array<uint32_t, 4> target{}, history{};
    };
    static_assert(sizeof(CascadeFrame) == 352);
    constexpr VkFormat radianceFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    void computeBarrier(VkCommandBuffer command) {
      const VkMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    uint32_t groups(uint32_t size) { return (size + 7) / 8; }
  } // namespace
  struct RadianceCascades::Gpu {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkExtent2D extent{};
    bool multisampled = false;
    uint32_t count = 0, spacing = 0, directions = 0; // cascade-0 pixels per probe, directions per side
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline prepare = VK_NULL_HANDLE, trace = VK_NULL_HANDLE, integrate = VK_NULL_HANDLE,
               gather = VK_NULL_HANDLE, filter = VK_NULL_HANDLE, cache = VK_NULL_HANDLE;
    std::unique_ptr<Buffer> cacheKeys, cacheSamples, cacheValues;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkSampler nearest = VK_NULL_HANDLE, linear = VK_NULL_HANDLE, clampedLinear = VK_NULL_HANDLE;
    std::unique_ptr<Image> geometry, normals, probeIrradiance, probeDirection, filtered, output;
    std::array<std::unique_ptr<Image>, maximumCascades> levels;
    std::array<std::unique_ptr<Image>, 2> history, counts; // ping-pong accumulation and its frame counts
    std::array<VkExtent2D, maximumCascades> levelExtent{};
    uint32_t written = 0; // the history image holding the latest gather
    bool initialized = false, hasOutput = false;
    uint32_t frameNumber = 0;
    Matrix outputCamera = scene::identity();
    uint32_t slot = 0;
    Statistics statistics;
    struct Frame {
      std::unique_ptr<Buffer> constants;
      std::array<VkDescriptorSet, maximumCascades> sets{}; // one per cascade: its image and its parent
    };
    std::array<Frame, gpuFrameCount> frames;
    uint64_t bytes = 0;
    ~Gpu() {
      if (pool)
        vkDestroyDescriptorPool(device, pool, nullptr);
      for (auto handle : {prepare, trace, integrate, gather, filter, cache})
        if (handle)
          pipeline::destroyPipeline(device, handle, nullptr);
      if (layout)
        pipeline::destroyPipelineLayout(device, layout, nullptr);
      if (setLayout)
        pipeline::destroyDescriptorSetLayout(device, setLayout, nullptr);
      for (auto handle : {nearest, linear, clampedLinear})
        if (handle)
          pipeline::destroySampler(device, handle, nullptr);
    }
    std::unique_ptr<Image> image(VkExtent2D size, VkFormat format, VkImageUsageFlags usage) {
      auto result = std::make_unique<Image>();
      result->create(physical, device, size, format, usage, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
      bytes += result->bytes;
      return result;
    }
  };
  RadianceCascades::RadianceCascades() = default;
  RadianceCascades::~RadianceCascades() = default;
  void RadianceCascades::initialize(VkPhysicalDevice physical, VkDevice device, VkExtent2D extent,
                                    VkSampleCountFlagBits samples, const Options &options,
                                    VkDescriptorSetLayout rayScene) {
    auto g = std::make_unique<Gpu>();
    g->physical = physical;
    g->device = device;
    g->extent = extent;
    g->multisampled = samples != VK_SAMPLE_COUNT_1_BIT;
    g->count = std::clamp(options.cascadeCount, 2u, maximumCascades);
    g->directions = options.cascadeDirections == 4 || options.cascadeDirections == 16
                        ? options.cascadeDirections : 8u;
    // Keep the probe count, and so the ray count, of about a 1080p target:
    // larger targets space probes further apart in pixels, not in angle.
    const uint32_t scale = std::max(1u, uint32_t((extent.height + 540) / 1080));
    g->spacing = g->directions * (scale >= 4 ? 4 : scale >= 2 ? 2 : 1);
    g->statistics.available = true;
    g->statistics.cascades = g->count;
    const auto storage = VK_IMAGE_USAGE_STORAGE_BIT;
    g->geometry = g->image(extent, VK_FORMAT_R32G32B32A32_SFLOAT, storage);
    g->normals = g->image(extent, radianceFormat, storage);
    for (uint32_t c = 0; c < g->count; ++c) {
      // Probes every spacing * 2^c pixels, each with directions * 2^c per side.
      const uint32_t size = g->spacing << c, sides = g->directions << c;
      const VkExtent2D probes{(extent.width + size - 1) / size, (extent.height + size - 1) / size};
      g->levelExtent[c] = {probes.width * sides, probes.height * sides};
      g->levels[c] = g->image(g->levelExtent[c], radianceFormat, storage);
      if (c == 0) {
        g->probeIrradiance = g->image(probes, radianceFormat, storage);
        g->probeDirection = g->image(probes, radianceFormat, storage | VK_IMAGE_USAGE_SAMPLED_BIT);
        g->statistics.probes = probes.width * probes.height;
      }
    }
    for (auto &target : g->history)
      target = g->image(extent, radianceFormat, storage);
    for (auto &target : g->counts)
      target = g->image(extent, VK_FORMAT_R32_SFLOAT, storage);
    g->filtered = g->image(extent, radianceFormat, storage);
    g->output = g->image(extent, radianceFormat, storage | VK_IMAGE_USAGE_SAMPLED_BIT);
    auto cacheBuffer = [&](VkDeviceSize stride) {
      auto result = std::make_unique<Buffer>();
      result->create(physical, device, stride * cacheEntries,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer::Memory::Device);
      g->bytes += result->allocationBytes;
      return result;
    };
    g->cacheKeys = cacheBuffer(sizeof(uint32_t));
    g->cacheSamples = cacheBuffer(4 * sizeof(uint32_t));
    g->cacheValues = cacheBuffer(4 * sizeof(float));

    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {12, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {17, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    const VkDescriptorSetLayoutCreateInfo setInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                  .bindingCount = uint32_t(std::size(bindings)),
                                                  .pBindings = bindings};
    check(pipeline::createDescriptorSetLayout(device, &setInfo, nullptr, &g->setLayout));
    const VkDescriptorSetLayout sets[] = {g->setLayout, rayScene};
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t)};
    const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                .setLayoutCount = 2,
                                                .pSetLayouts = sets,
                                                .pushConstantRangeCount = 1,
                                                .pPushConstantRanges = &push};
    check(pipeline::createPipelineLayout(device, &layoutInfo, nullptr, &g->layout));
    // Compile together so the preparation service can build them concurrently.
    const std::array<std::pair<const uint32_t *, size_t>, 6> code = {
        g->multisampled ? std::pair{prepareShader, sizeof(prepareShader)}
                        : std::pair{prepareSingleShader, sizeof(prepareSingleShader)},
        std::pair{traceShader, sizeof(traceShader)}, std::pair{integrateShader, sizeof(integrateShader)},
        std::pair{gatherShader, sizeof(gatherShader)}, std::pair{filterShader, sizeof(filterShader)},
        std::pair{cacheShader, sizeof(cacheShader)}};
    std::array<VkComputePipelineCreateInfo, 6> recipes{};
    std::array<VkPipeline, 6> pipelines{};
    for (size_t i = 0; i < code.size(); ++i)
      recipes[i] = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                    .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                              .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                              .module = shaderModule(device, code[i].first, code[i].second),
                              .pName = "main"},
                    .layout = g->layout};
    const auto status = pipeline::createComputePipelines(device, VK_NULL_HANDLE, uint32_t(recipes.size()),
                                                         recipes.data(), nullptr, pipelines.data());
    for (const auto &recipe : recipes)
      pipeline::destroyShaderModule(device, recipe.stage.module, nullptr);
    g->prepare = pipelines[0];
    g->trace = pipelines[1];
    g->integrate = pipelines[2];
    g->gather = pipelines[3];
    g->filter = pipelines[4];
    g->cache = pipelines[5];
    check(status);
    VkSamplerCreateInfo samplerInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                    .magFilter = VK_FILTER_NEAREST,
                                    .minFilter = VK_FILTER_NEAREST,
                                    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                    .maxLod = 0};
    check(pipeline::createSampler(device, &samplerInfo, nullptr, &g->nearest));
    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
    check(pipeline::createSampler(device, &samplerInfo, nullptr, &g->clampedLinear));
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    check(pipeline::createSampler(device, &samplerInfo, nullptr, &g->linear));
    const uint32_t setCount = gpuFrameCount * maximumCascades;
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, setCount},
                                          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, setCount * 2},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, setCount * 12},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, setCount * 3}};
    const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .maxSets = setCount,
                                              .poolSizeCount = uint32_t(std::size(sizes)),
                                              .pPoolSizes = sizes};
    check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &g->pool));
    for (auto &frame : g->frames) {
      frame.constants = std::make_unique<Buffer>();
      frame.constants->create(physical, device, sizeof(CascadeFrame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
      std::array<VkDescriptorSetLayout, maximumCascades> layouts;
      layouts.fill(g->setLayout);
      const VkDescriptorSetAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                   .descriptorPool = g->pool,
                                                   .descriptorSetCount = g->count,
                                                   .pSetLayouts = layouts.data()};
      check(vkAllocateDescriptorSets(device, &allocation, frame.sets.data()));
    }
    m_gpu = std::move(g);
  }
  void RadianceCascades::selectFrame(uint32_t slot) { m_gpu->slot = slot; }
  void RadianceCascades::record(VkCommandBuffer command, const Inputs &inputs, const Options &options) {
    auto &g = *m_gpu;
    auto &frame = g.frames[g.slot];
    gpu::GpuScope time(command, "Forward+ radiance cascades", g.extent);
    // Targets are shared by every slot: the previous frame's shading and
    // feedback reads precede this frame's writes on the queue.
    const VkMemoryBarrier reads{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &reads, 0, nullptr, 0, nullptr);
    if (!g.initialized) {
      std::vector<VkImageMemoryBarrier> barriers;
      auto general = [&](const std::unique_ptr<Image> &image) {
        if (image)
          barriers.push_back({.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = image->handle,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}});
      };
      for (const auto *image : {&g.geometry, &g.normals, &g.probeIrradiance, &g.probeDirection, &g.history[0],
                                &g.history[1], &g.counts[0], &g.counts[1], &g.filtered, &g.output})
        general(*image);
      for (const auto &level : g.levels)
        general(level);
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                           nullptr, 0, nullptr, uint32_t(barriers.size()), barriers.data());
      // Empty cache: zero keys and samples; values -1 mark cells not yet resolved.
      vkCmdFillBuffer(command, g.cacheKeys->handle, 0, VK_WHOLE_SIZE, 0);
      vkCmdFillBuffer(command, g.cacheSamples->handle, 0, VK_WHOLE_SIZE, 0);
      vkCmdFillBuffer(command, g.cacheValues->handle, 0, VK_WHOLE_SIZE, 0xbf800000u);
      const VkMemoryBarrier filled{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                   .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                   .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                           &filled, 0, nullptr, 0, nullptr);
      g.initialized = true;
    }
    const uint32_t previous = g.written, next = 1 - g.written;
    const float scale = options.sceneScale;
    CascadeFrame constants{scene::inverse(inputs.camera), inputs.view, g.outputCamera, inputs.worldToDome};
    constants.eye = {inputs.eye[0], inputs.eye[1], inputs.eye[2], 1};
    constants.interval = {options.cascadeInterval / scale, .002f / scale, options.cascadeDistance / scale, .05f};
    // History weight h keeps a running mean over up to 1 / (1 - h) frames.
    const float frames = 1 / (1 - std::clamp(options.cascadeHistory, 0.f, .98f));
    constants.control = {frames, options.cascadeBounces ? 1.f : 0.f, cacheCellMetres / scale, 0};
    constants.skyRadiance = inputs.skyRadiance;
    constants.target = {g.extent.width, g.extent.height, g.count, g.spacing};
    constants.history = {g.hasOutput ? 1u : 0u, g.frameNumber++, g.directions, 0};
    frame.constants->write(&constants, sizeof(constants));
    const auto storage = [](const std::unique_ptr<Image> &image) {
      return VkDescriptorImageInfo{VK_NULL_HANDLE, image->view, VK_IMAGE_LAYOUT_GENERAL};
    };
    const VkDescriptorBufferInfo buffer = frame.constants->descriptor();
    const VkDescriptorImageInfo depth{g.nearest, inputs.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo sky = inputs.sky;
    sky.sampler = g.linear;
    for (uint32_t c = 0; c < g.count; ++c) {
      // The filter's two passes use sets 0 (accumulation to scratch) and 1
      // (scratch to the output shading reads).
      const VkDescriptorImageInfo images[] = {depth,
                                              storage(g.geometry),
                                              storage(g.normals),
                                              storage(g.levels[c]),
                                              storage(g.history[previous]),
                                              storage(g.history[next]),
                                              sky,
                                              storage(g.probeIrradiance),
                                              storage(g.levels[std::min(c + 1, g.count - 1)]),
                                              storage(g.counts[previous]),
                                              storage(g.counts[next]),
                                              storage(c == 1 ? g.filtered : g.history[next]),
                                              storage(c == 1 ? g.output : g.filtered)};
      VkWriteDescriptorSet writes[14]{};
      writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                   .dstSet = frame.sets[c],
                   .dstBinding = 0,
                   .descriptorCount = 1,
                   .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                   .pBufferInfo = &buffer};
      for (uint32_t i = 1; i < 14; ++i)
        writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstSet = frame.sets[c],
                     .dstBinding = i,
                     .descriptorCount = 1,
                     .descriptorType = i == 1 || i == 7 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                        : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                     .pImageInfo = &images[i - 1]};
      vkUpdateDescriptorSets(g.device, 14, writes, 0, nullptr);
      const VkDescriptorBufferInfo cache[] = {g.cacheKeys->descriptor(), g.cacheSamples->descriptor(),
                                              g.cacheValues->descriptor()};
      VkWriteDescriptorSet cacheWrites[3];
      for (uint32_t i = 0; i < 3; ++i)
        cacheWrites[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                          .dstSet = frame.sets[c],
                          .dstBinding = 14 + i,
                          .descriptorCount = 1,
                          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          .pBufferInfo = &cache[i]};
      vkUpdateDescriptorSets(g.device, 3, cacheWrites, 0, nullptr);
      const auto direction = storage(g.probeDirection);
      const VkWriteDescriptorSet directionWrite{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                .dstSet = frame.sets[c],
                                                .dstBinding = 17,
                                                .descriptorCount = 1,
                                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                                .pImageInfo = &direction};
      vkUpdateDescriptorSets(g.device, 1, &directionWrite, 0, nullptr);
    }
    auto bind = [&](VkPipeline pipeline, uint32_t c) {
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      const VkDescriptorSet sets[] = {frame.sets[c], inputs.rayScene};
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0, 2, sets, 0, nullptr);
      vkCmdPushConstants(command, g.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(c), &c);
    };
    bind(g.prepare, 0);
    vkCmdDispatch(command, groups(g.extent.width), groups(g.extent.height), 1);
    computeBarrier(command);
    g.statistics.rays = 0;
    static constexpr const char *traceNames[maximumCascades] = {
        "Forward+ cascade 0 trace", "Forward+ cascade 1 trace", "Forward+ cascade 2 trace",
        "Forward+ cascade 3 trace", "Forward+ cascade 4 trace", "Forward+ cascade 5 trace"};
    for (uint32_t c = g.count; c-- > 0;) {
      gpu::GpuScope level(command, traceNames[c], g.levelExtent[c]);
      bind(g.trace, c);
      vkCmdDispatch(command, groups(g.levelExtent[c].width), groups(g.levelExtent[c].height), 1);
      computeBarrier(command);
      g.statistics.rays += uint64_t(g.levelExtent[c].width) * g.levelExtent[c].height;
    }
    bind(g.integrate, 0);
    const uint32_t probesWide = (g.extent.width + g.spacing - 1) / g.spacing,
                   probesHigh = (g.extent.height + g.spacing - 1) / g.spacing;
    vkCmdDispatch(command, groups(probesWide), groups(probesHigh), 1);
    {
      // Independent of integration: this frame's hits become next frame's cache.
      gpu::GpuScope resolve(command, "Forward+ cascade world cache");
      bind(g.cache, 0);
      vkCmdDispatch(command, cacheEntries / 64, 1, 1);
    }
    computeBarrier(command);
    bind(g.gather, 0);
    vkCmdDispatch(command, groups(g.extent.width), groups(g.extent.height), 1);
    computeBarrier(command);
    {
      gpu::GpuScope filtering(command, "Forward+ cascade filter", g.extent);
      for (uint32_t pass = 0; pass < 2; ++pass) {
        const uint32_t step = 2u << pass; // 2 then 4 pixels: inside the probe spacing
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, g.filter);
        const VkDescriptorSet sets[] = {frame.sets[pass], inputs.rayScene};
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0, 2, sets, 0, nullptr);
        vkCmdPushConstants(command, g.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(step), &step);
        vkCmdDispatch(command, groups(g.extent.width), groups(g.extent.height), 1);
        if (pass == 0)
          computeBarrier(command);
      }
    }
    // The next frame's world shading samples the filtered result.
    const VkMemoryBarrier written{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                  .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                  .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &written, 0, nullptr, 0, nullptr);
    g.written = next;
    g.hasOutput = true;
    g.outputCamera = inputs.camera;
    g.statistics.active = true;
    std::snprintf(g.statistics.status, sizeof(g.statistics.status),
                  "Radiance cascades active: %u cascades, %u px probes", g.count, g.spacing);
  }
  bool RadianceCascades::hasOutput() const { return m_gpu->hasOutput; }
  VkDescriptorImageInfo RadianceCascades::output() const {
    return {m_gpu->nearest, m_gpu->output->view, VK_IMAGE_LAYOUT_GENERAL};
  }
  VkDescriptorImageInfo RadianceCascades::directions() const {
    return {m_gpu->clampedLinear, m_gpu->probeDirection->view, VK_IMAGE_LAYOUT_GENERAL};
  }
  uint32_t RadianceCascades::probeSpacing() const { return m_gpu->spacing; }
  const Matrix &RadianceCascades::outputCamera() const { return m_gpu->outputCamera; }
  const Statistics &RadianceCascades::statistics() const { return m_gpu->statistics; }
  uint64_t RadianceCascades::targetBytes() const { return m_gpu->bytes; }
} // namespace remix::forward::cascades
