#include "forward/ray/cascades.h"
#include "forward/bake/transport.h"
#include "forward/ray/cascade_shaders/embedded.h"
#include "forward/resources.h"
#include "platform/gpu_frames.h"
#include "platform/gpu_timers.h"
#include "runtime/pipelines.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace remix::forward::ray {
  namespace {
    // std140 mirror of CascadeFrameBlock in cascade_shaders/common.glsl.
    struct CascadeFrame {
      Matrix inverseCamera{};
      Vec4 cameraSpacing{}, ranges{};
      std::array<uint32_t, 4> target{}, config{}, sequence{};
    };
    static_assert(sizeof(CascadeFrame) == 144);
    constexpr uint32_t tableSize = 1u << Cascades::tableBits;
    constexpr uint32_t directions(uint32_t cascade) { return Cascades::baseDirections << (2 * cascade); }
    constexpr uint32_t probeBase(uint32_t cascade) {
      uint32_t base = 0;
      for (uint32_t i = 0; i < cascade; ++i) base += Cascades::capacities[i];
      return base;
    }
    constexpr uint64_t binBase(uint32_t cascade) {
      uint64_t base = 0;
      for (uint32_t i = 0; i < cascade; ++i) base += uint64_t(Cascades::capacities[i]) * directions(i);
      return base;
    }
    // Cascade 0 keeps its own directions; a higher cascade those of the cascade below.
    constexpr uint32_t coneEntries(uint32_t cascade) {
      return Cascades::capacities[cascade] * directions(cascade ? cascade - 1 : 0);
    }
    constexpr uint64_t coneCount() {
      uint64_t count = 0;
      for (uint32_t i = 0; i < Cascades::cascadeCount; ++i) count += coneEntries(i);
      return count;
    }
    constexpr uint32_t probeCount = probeBase(Cascades::cascadeCount);
    constexpr uint32_t baseProbes = Cascades::capacities[0];
    constexpr VkExtent2D atlasExtent{Cascades::tileRow * Cascades::tile,
                                     baseProbes / Cascades::tileRow * Cascades::tile};
    // The keys wrap at 256 cells; leave room for a probe's trilinear neighbours.
    static_assert(Cascades::maximumLodCells + 2 < 128);
    static_assert(atlasExtent.width == 2048 && atlasExtent.height == 1024);

    constexpr VkAccessFlags shaderAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    void memoryBarrier(VkCommandBuffer command, VkPipelineStageFlags sourceStage, VkAccessFlags sourceAccess,
                       VkPipelineStageFlags destinationStage, VkAccessFlags destinationAccess) {
      const VkMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                    .srcAccessMask = sourceAccess,
                                    .dstAccessMask = destinationAccess};
      vkCmdPipelineBarrier(command, sourceStage, destinationStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    void computeBarrier(VkCommandBuffer command) {
      memoryBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, shaderAccess,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, shaderAccess);
    }
    VkDescriptorSetLayout queryLayout(VkDevice device) {
      const VkDescriptorSetLayoutBinding bindings[] = {
          {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
          {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
          {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
      const VkDescriptorSetLayoutCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .bindingCount = uint32_t(std::size(bindings)),
                                                 .pBindings = bindings};
      VkDescriptorSetLayout layout;
      check(pipeline::createDescriptorSetLayout(device, &info, nullptr, &layout));
      return layout;
    }
    VkDescriptorType buildBindingType(uint32_t binding) {
      if (binding == 0) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      if (binding == 2 || binding == 3 || binding == 12) return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      if (binding == 9) return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    constexpr uint32_t buildBindings = 17;
  } // namespace

  struct Cascades::Gpu {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    LiveScene *scene = nullptr;
    VkDescriptorSetLayout buildSetLayout = VK_NULL_HANDLE, querySetLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline insert = VK_NULL_HANDLE, launch = VK_NULL_HANDLE, deposit = VK_NULL_HANDLE,
               summary = VK_NULL_HANDLE, link = VK_NULL_HANDLE, merge = VK_NULL_HANDLE,
               irradiance = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkSampler guideSampler = VK_NULL_HANDLE, atlasSampler = VK_NULL_HANDLE;
    // One build, shared by every frame slot: commands on the renderer's queue
    // execute in submission order, and only the constants are written by the CPU.
    Buffer table, historyTable, counters, cells, bins, links, cones, summaries;
    // The published build and the one before it alternate between two sets of
    // irradiance tiles and probe ages; a build blends the other set's history.
    std::array<Image, 2> atlases;
    std::array<Buffer, 2> ages;
    uint32_t publishedAtlas = 0;
    bool atlasInitialized = false;
    std::optional<CascadeFrame> published;
    uint32_t slot = 0;
    uint64_t builds = 0;
    // Finest cells per level of detail; lowered while a probe pool overflows.
    float lodCells = 0, requestedLodCells = 0;
    uint32_t settledBuilds = 0;
    std::array<uint32_t, cascadeCount> usage{};
    Statistics statistics;
    struct Frame {
      Buffer buildConstants, queryConstants, countersReadback;
      VkDescriptorSet build = VK_NULL_HANDLE, query = VK_NULL_HANDLE;
      bool recorded = false;
      uint32_t rays = 0, bounces = 0;
      float lodCells = 0;
    };
    std::array<Frame, gpuFrameCount> frames;
    void createResources();
    void adapt(const Frame &frame);
    void atlasBarrier(VkCommandBuffer command, const Image &image, VkPipelineStageFlags sourceStage,
                      VkAccessFlags sourceAccess, VkPipelineStageFlags destinationStage,
                      VkAccessFlags destinationAccess, VkImageLayout oldLayout = VK_IMAGE_LAYOUT_GENERAL) const {
      const VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                         .srcAccessMask = sourceAccess,
                                         .dstAccessMask = destinationAccess,
                                         .oldLayout = oldLayout,
                                         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .image = image.handle,
                                         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      vkCmdPipelineBarrier(command, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    ~Gpu() {
      if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
      for (auto handle : {insert, launch, deposit, summary, link, merge, irradiance})
        if (handle) pipeline::destroyPipeline(device, handle, nullptr);
      if (layout) pipeline::destroyPipelineLayout(device, layout, nullptr);
      for (auto handle : {buildSetLayout, querySetLayout})
        if (handle) pipeline::destroyDescriptorSetLayout(device, handle, nullptr);
      for (auto handle : {guideSampler, atlasSampler})
        if (handle) pipeline::destroySampler(device, handle, nullptr);
    }
  };
  Cascades::Cascades() = default;
  Cascades::~Cascades() = default;
  VkDescriptorSetLayout Cascades::createQueryLayout(VkDevice device) { return queryLayout(device); }
  void Cascades::initialize(VkPhysicalDevice physical, VkDevice device, LiveScene &scene) {
    auto next = std::make_unique<Gpu>();
    next->physical = physical;
    next->device = device;
    next->scene = &scene;
    next->statistics.available = true;
    next->createResources();
    m_gpu = std::move(next);
  }
  void Cascades::Gpu::createResources() {
    auto storage = [&](Buffer &buffer, uint64_t bytes, VkBufferUsageFlags usage = 0) {
      buffer.create(physical, device, bytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
                    Buffer::Memory::Device);
    };
    const uint64_t tableBytes = uint64_t(tableSize) * 2 * sizeof(uint32_t);
    storage(table, tableBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    storage(historyTable, tableBytes);
    storage(counters, cascadeCount * sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    storage(cells, uint64_t(probeCount) * 4 * sizeof(int32_t));
    storage(bins, binBase(cascadeCount) * 4 * sizeof(uint32_t));
    storage(links, uint64_t(probeCount) * 8 * sizeof(uint32_t));
    storage(cones, coneCount() * 2 * sizeof(uint32_t));
    storage(summaries, uint64_t(probeCount) * 2 * sizeof(uint32_t));
    for (auto &age : ages) storage(age, uint64_t(baseProbes) * sizeof(uint32_t));
    for (auto &atlas : atlases)
      atlas.create(physical, device, atlasExtent, VK_FORMAT_R16G16B16A16_SFLOAT,
                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SAMPLE_COUNT_1_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT);

    VkDescriptorSetLayoutBinding bindings[buildBindings];
    for (uint32_t i = 0; i < buildBindings; ++i)
      bindings[i] = {i, buildBindingType(i), 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    const VkDescriptorSetLayoutCreateInfo setInfo{.sType =
                                                      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                  .bindingCount = buildBindings,
                                                  .pBindings = bindings};
    check(pipeline::createDescriptorSetLayout(device, &setInfo, nullptr, &buildSetLayout));
    querySetLayout = queryLayout(device);
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                .setLayoutCount = 1,
                                                .pSetLayouts = &buildSetLayout,
                                                .pushConstantRangeCount = 1,
                                                .pPushConstantRanges = &push};
    check(pipeline::createPipelineLayout(device, &layoutInfo, nullptr, &layout));
    auto compute = [&](const uint32_t *code, size_t bytes) {
      const auto module = shaderModule(device, code, bytes);
      const VkComputePipelineCreateInfo info{
          .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = module,
                    .pName = "main"},
          .layout = layout};
      VkPipeline result;
      const auto status =
          pipeline::createComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &result);
      pipeline::destroyShaderModule(device, module, nullptr);
      check(status);
      return result;
    };
    insert = compute(cascades::insertShader, sizeof(cascades::insertShader));
    launch = compute(cascades::launchShader, sizeof(cascades::launchShader));
    deposit = compute(cascades::depositShader, sizeof(cascades::depositShader));
    summary = compute(cascades::summaryShader, sizeof(cascades::summaryShader));
    link = compute(cascades::linkShader, sizeof(cascades::linkShader));
    merge = compute(cascades::mergeShader, sizeof(cascades::mergeShader));
    irradiance = compute(cascades::irradianceShader, sizeof(cascades::irradianceShader));

    VkSamplerCreateInfo samplerInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                    .magFilter = VK_FILTER_NEAREST,
                                    .minFilter = VK_FILTER_NEAREST,
                                    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    check(pipeline::createSampler(device, &samplerInfo, nullptr, &guideSampler));
    // Bilinear within a probe's tile; its border keeps neighbouring tiles out.
    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
    check(pipeline::createSampler(device, &samplerInfo, nullptr, &atlasSampler));

    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, gpuFrameCount * 2},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, gpuFrameCount * 13},
                                          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, gpuFrameCount * 4},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, gpuFrameCount}};
    const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .maxSets = gpuFrameCount * 2,
                                              .poolSizeCount = uint32_t(std::size(sizes)),
                                              .pPoolSizes = sizes};
    check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool));
    for (auto &frame : frames) {
      frame.buildConstants.create(physical, device, sizeof(CascadeFrame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
      frame.queryConstants.create(physical, device, sizeof(CascadeFrame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
      frame.countersReadback.create(physical, device, cascadeCount * sizeof(uint32_t),
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer::Memory::Readback);
      const VkDescriptorSetLayout layouts[] = {buildSetLayout, querySetLayout};
      VkDescriptorSet sets[2];
      const VkDescriptorSetAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                   .descriptorPool = pool,
                                                   .descriptorSetCount = 2,
                                                   .pSetLayouts = layouts};
      check(vkAllocateDescriptorSets(device, &allocation, sets));
      frame.build = sets[0];
      frame.query = sets[1];
      // The guides, the transport's batches and the alternating atlas and ages are written per frame.
      const VkDescriptorBufferInfo buffers[] = {frame.buildConstants.descriptor(), table.descriptor(),
                                                counters.descriptor(),             cells.descriptor(),
                                                bins.descriptor(),                 links.descriptor(),
                                                cones.descriptor(),                historyTable.descriptor(),
                                                summaries.descriptor(),            frame.queryConstants.descriptor()};
      const uint32_t buildBuffers[] = {0, 1, 4, 5, 6, 7, 8, 13, 16};
      VkWriteDescriptorSet writes[11]{};
      for (uint32_t i = 0; i < 9; ++i)
        writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstSet = frame.build,
                     .dstBinding = buildBuffers[i],
                     .descriptorCount = 1,
                     .descriptorType = buildBindingType(buildBuffers[i]),
                     .pBufferInfo = &buffers[i]};
      writes[9] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                   .dstSet = frame.query,
                   .dstBinding = 0,
                   .descriptorCount = 1,
                   .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                   .pBufferInfo = &buffers[9]};
      writes[10] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = frame.query,
                    .dstBinding = 1,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .pBufferInfo = &buffers[1]};
      vkUpdateDescriptorSets(device, uint32_t(std::size(writes)), writes, 0, nullptr);
    }
  }
  // Probe counts of a completed build. A pool that overflowed dropped probes,
  // which shows as dark cells: coarsen every level until the build fits, and
  // return towards the requested density once it has room again.
  void Cascades::Gpu::adapt(const Frame &frame) {
    std::array<uint32_t, cascadeCount> counts{};
    frame.countersReadback.read(counts.data(), sizeof(counts));
    usage = counts;
    float fullest = 0;
    for (uint32_t cascade = 0; cascade < cascadeCount; ++cascade)
      fullest = std::max(fullest, float(counts[cascade]) / float(capacities[cascade]));
    std::snprintf(statistics.status, sizeof(statistics.status),
                  "Cascades: %u rays, %u bounce%s; probes %u/%u, %u/%u, %u/%u, %u/%u; density %.0f%s",
                  frame.rays, frame.bounces, frame.bounces == 1 ? "" : "s", counts[0], capacities[0], counts[1],
                  capacities[1], counts[2], capacities[2], counts[3], capacities[3], frame.lodCells,
                  frame.lodCells < requestedLodCells ? " (reduced to fit)" : "");
    // Builds recorded before this density took effect do not judge it.
    if (frame.lodCells != lodCells) return;
    if (fullest > .9f && lodCells > minimumLodCells) {
      lodCells = std::max(std::floor(lodCells * (fullest > 1.5f ? .7f : .85f)), minimumLodCells);
      settledBuilds = 0;
    } else if (fullest < .45f && lodCells < requestedLodCells && ++settledBuilds > 240) {
      lodCells = std::min(std::ceil(lodCells * 1.15f), requestedLodCells);
      settledBuilds = 0;
    }
  }
  void Cascades::selectFrame(uint32_t slot) {
    auto &g = *m_gpu;
    g.slot = slot;
    auto &frame = g.frames[slot];
    if (frame.recorded) g.adapt(frame);
    frame.recorded = false;
  }
  VkDescriptorSet Cascades::prepareQuery(VkCommandBuffer command) {
    auto &g = *m_gpu;
    auto &frame = g.frames[g.slot];
    if (!g.atlasInitialized) {
      // Receivers bind an atlas before the first build; it is not sampled until one is published.
      for (const auto &atlas : g.atlases)
        g.atlasBarrier(command, atlas, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       shaderAccess, VK_IMAGE_LAYOUT_UNDEFINED);
      g.atlasInitialized = true;
    }
    const CascadeFrame constants = g.published.value_or(CascadeFrame{});
    frame.queryConstants.write(&constants, sizeof(constants));
    const VkDescriptorImageInfo atlas{g.atlasSampler, g.atlases[g.publishedAtlas].view, VK_IMAGE_LAYOUT_GENERAL};
    const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .dstSet = frame.query,
                                     .dstBinding = 2,
                                     .descriptorCount = 1,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                     .pImageInfo = &atlas};
    vkUpdateDescriptorSets(g.device, 1, &write, 0, nullptr);
    return frame.query;
  }
  void Cascades::record(VkCommandBuffer command, Inputs inputs, const FrameData &source, const Options &options,
                        uint32_t bounces) {
    auto &g = *m_gpu;
    auto &frame = g.frames[g.slot];
    const auto sceneStatistics = g.scene->statistics();
    g.statistics.available = true;
    g.statistics.sceneBuilds = sceneStatistics.sceneBuilds;
    g.statistics.rays = 0;
    auto *transport = g.scene->transport();
    if (!transport || !g.atlasInitialized || !inputs.extent.width || !inputs.extent.height) {
      g.statistics.active = false;
      std::snprintf(g.statistics.status, sizeof(g.statistics.status), "%s", sceneStatistics.status);
      return;
    }
    const auto batch = transport->deviceBatch(g.slot);
    if (batch.capacity < 64) {
      g.statistics.active = false;
      std::snprintf(g.statistics.status, sizeof(g.statistics.status),
                    "Radiance cascades need the live scene's ray batches");
      return;
    }
    const float requested = std::clamp(float(options.cascadeLodCells), minimumLodCells, maximumLodCells);
    if (requested != g.requestedLodCells) {
      g.requestedLodCells = g.lodCells = requested;
      g.settledBuilds = 0;
    }
    const uint32_t stride = std::clamp(options.cascadeRayStride, 1u, 8u);
    const VkExtent2D grid{(inputs.extent.width + stride - 1) / stride, (inputs.extent.height + stride - 1) / stride};
    const float spacing = options.cascadeSpacing / options.sceneScale;
    const auto sequence = uint32_t(g.builds);
    CascadeFrame constants;
    constants.inverseCamera = scene::inverse(source.camera);
    constants.cameraSpacing = {source.cameraPosition[0], source.cameraPosition[1], source.cameraPosition[2],
                               spacing};
    constants.ranges = {g.lodCells * spacing, intervalCells * spacing, .002f / options.sceneScale, radianceScale};
    constants.target = {inputs.extent.width, inputs.extent.height, grid.width, grid.height};
    constants.config = {stride, batch.capacity, 1, 0};
    // Probes keep their keys only while the grid they are keyed on is unchanged.
    const bool history = options.cascadeAccumulation && g.published &&
                         g.published->cameraSpacing[3] == spacing && g.published->ranges[0] == constants.ranges[0];
    // With accumulation, each build shifts the direction sequence and samples
    // another pixel of every stride block. Without it the pattern stays put:
    // changing noise that nothing averages would only flicker.
    const bool vary = options.cascadeAccumulation;
    constants.sequence = {vary ? sequence * 7919u : 0u, vary ? sequence % stride : stride / 2,
                          vary ? (sequence / stride) % stride : stride / 2, history ? 1u : 0u};
    frame.buildConstants.write(&constants, sizeof(constants));
    const uint32_t write = g.published ? 1 - g.publishedAtlas : g.publishedAtlas, read = 1 - write;
    const VkDescriptorImageInfo images[] = {{g.guideSampler, inputs.normals, VK_IMAGE_LAYOUT_GENERAL},
                                            {g.guideSampler, inputs.depth, VK_IMAGE_LAYOUT_GENERAL},
                                            {VK_NULL_HANDLE, g.atlases[write].view, VK_IMAGE_LAYOUT_GENERAL},
                                            {g.guideSampler, g.atlases[read].view, VK_IMAGE_LAYOUT_GENERAL}};
    const VkDescriptorBufferInfo buffers[] = {batch.points, batch.results, g.ages[write].descriptor(),
                                              g.ages[read].descriptor()};
    const uint32_t imageBindings[] = {2, 3, 9, 12}, bufferBindings[] = {10, 11, 14, 15};
    VkWriteDescriptorSet writes[8]{};
    for (uint32_t i = 0; i < 4; ++i) {
      writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                   .dstSet = frame.build,
                   .dstBinding = imageBindings[i],
                   .descriptorCount = 1,
                   .descriptorType = buildBindingType(imageBindings[i]),
                   .pImageInfo = &images[i]};
      writes[4 + i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                       .dstSet = frame.build,
                       .dstBinding = bufferBindings[i],
                       .descriptorCount = 1,
                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                       .pBufferInfo = &buffers[i]};
    }
    vkUpdateDescriptorSets(g.device, 8, writes, 0, nullptr);

    gpu::GpuScope time(command, "Forward+ radiance cascades", inputs.extent);
    // Extra timestamps are opt-in, like the reflection stages'.
    static const bool profile = std::getenv("REMIX_DEBUG_CASCADE_TIMINGS") != nullptr;
    std::optional<gpu::GpuScope> stageTime;
    auto stage = [&](const char *name) {
      stageTime.reset();
      if (profile && gpu::timing()) stageTime.emplace(command, name);
    };
    stage("Cascades clear");
    // This frame's receivers have finished reading the previous build.
    const auto readers = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    memoryBarrier(command, readers, shaderAccess, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    g.atlasBarrier(command, g.atlases[write], readers, shaderAccess, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   shaderAccess);
    // Keep the previous build's table for its history, then empty the keys,
    // probe counts and direction bins. Probe indices, cells, links, cones and
    // tiles are only reached through this build's keys.
    if (history) {
      const VkBufferCopy whole{0, 0, g.table.capacity};
      vkCmdCopyBuffer(command, g.table.handle, g.historyTable.handle, 1, &whole);
      memoryBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    }
    vkCmdFillBuffer(command, g.table.handle, 0, VkDeviceSize(tableSize) * sizeof(uint32_t), 0);
    vkCmdFillBuffer(command, g.counters.handle, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(command, g.bins.handle, 0, VK_WHOLE_SIZE, 0);
    memoryBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, shaderAccess);

    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0, 1, &frame.build, 0, nullptr);
    auto dispatch = [&](VkPipeline pipeline, uint32_t groupsX, uint32_t groupsY, uint32_t begin = 0,
                        uint32_t count = 0, uint32_t level = 0) {
      const uint32_t push[] = {begin, count, level, 0};
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdPushConstants(command, g.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
      vkCmdDispatch(command, groupsX, groupsY, 1);
      computeBarrier(command);
    };
    stage("Cascades insert");
    dispatch(g.insert, (inputs.extent.width + 7) / 8, (inputs.extent.height + 7) / 8);
    stage("Cascades launch, trace and deposit");
    const uint64_t rays64 = uint64_t(grid.width) * grid.height;
    if (rays64 > UINT32_MAX) throw VK_ERROR_TOO_MANY_OBJECTS;
    const auto rays = uint32_t(rays64);
    for (uint32_t begin = 0; begin < rays;) {
      const auto size = std::min(batch.capacity, rays - begin);
      // Every batch shares the slot's constants: pad the last one with inactive
      // rays instead of changing its count.
      dispatch(g.launch, (batch.capacity + 63) / 64, 1, begin, size);
      transport->recordDeviceRadiance(command, g.slot, batch.capacity, bounces);
      // The transport bound its own pipeline layout and set.
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0, 1, &frame.build, 0,
                              nullptr);
      dispatch(g.deposit, (size + 63) / 64, 1, begin, size);
      begin += size;
    }
    stage("Cascades link and merge");
    dispatch(g.summary, (probeCount + 63) / 64, 1);
    dispatch(g.link, (probeBase(cascadeCount - 1) + 63) / 64, 1);
    for (uint32_t cascade = cascadeCount; cascade-- > 0;)
      dispatch(g.merge, (coneEntries(cascade) + 63) / 64, 1, 0, 0, cascade);
    stage("Cascades irradiance");
    dispatch(g.irradiance, baseProbes, 1);
    stageTime.reset();
    memoryBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, shaderAccess,
                  readers | VK_PIPELINE_STAGE_TRANSFER_BIT, shaderAccess | VK_ACCESS_TRANSFER_READ_BIT);
    g.atlasBarrier(command, g.atlases[write], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, shaderAccess, readers,
                   shaderAccess);
    // Probe counts return with this slot's fence.
    const VkBufferCopy counts{0, 0, cascadeCount * sizeof(uint32_t)};
    vkCmdCopyBuffer(command, g.counters.handle, frame.countersReadback.handle, 1, &counts);
    memoryBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    g.published = constants;
    g.publishedAtlas = write;
    ++g.builds;
    frame.recorded = true;
    frame.rays = rays;
    frame.bounces = bounces;
    frame.lodCells = g.lodCells;
    g.statistics.active = true;
    g.statistics.rays = rays;
  }
  const Statistics &Cascades::statistics() const { return m_gpu->statistics; }
  Cascades::Memory Cascades::memory() const {
    const auto &g = *m_gpu;
    Memory memory;
    for (const auto *buffer : {&g.table, &g.historyTable, &g.counters, &g.cells, &g.bins, &g.links, &g.cones,
                               &g.summaries, &g.ages[0], &g.ages[1]})
      memory.buffers += buffer->allocationBytes;
    for (const auto &frame : g.frames)
      memory.buffers += frame.buildConstants.allocationBytes + frame.queryConstants.allocationBytes +
                        frame.countersReadback.allocationBytes;
    for (const auto &atlas : g.atlases) memory.images += atlas.bytes;
    return memory;
  }
} // namespace remix::forward::ray
