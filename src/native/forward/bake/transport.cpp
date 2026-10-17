#include "forward/bake/gpu.h"
#include "runtime/pipelines.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace remix::forward::bake {
  namespace {
    Frame lightingFrame(const scene::DistantLight &sun, const scene::DomeLight &dome, float distance,
                        float bias, float sceneScale) {
      Frame frame;
      frame.dome = dome.worldToLocal;
      std::copy_n(sun.direction, 3, frame.sunDirection.begin());
      frame.sunDirection[3] = sun.angularDiameterDegrees * std::numbers::pi_v<float> / 360;
      if (sun.isDistant)
        std::copy_n(sun.radiance, 3, frame.sunRadiance.begin());
      if (dome.enabled)
        std::copy_n(dome.radiance.begin(), 3, frame.domeRadiance.begin());
      frame.trace = {distance / sceneScale, bias / sceneScale, 0, 0};
      return frame;
    }
  } // namespace
  Transport::Transport() = default;
  Transport::~Transport() = default;
  void Transport::preparePipelines(VkDevice device) {
    Gpu pipelines;
    pipelines.device = device;
    pipelines.createLayouts();
    pipelines.createPipelines();
  }
  Transport::Gpu::~Gpu() {
    if (pool)
      vkDestroyDescriptorPool(device, pool, nullptr);
    for (auto handle : {tracePipeline, parallelPipeline, visibilityPipeline, atlasPipeline})
      if (handle)
        pipeline::destroyPipeline(device, handle, nullptr);
    for (auto handle : {layout, atlasLayout})
      if (handle)
        pipeline::destroyPipelineLayout(device, handle, nullptr);
    for (auto handle : {setLayout, atlasSetLayout})
      if (handle)
        pipeline::destroyDescriptorSetLayout(device, handle, nullptr);
    if (sampler)
      pipeline::destroySampler(device, sampler, nullptr);
    for (auto view : atlasViews)
      vkDestroyImageView(device, view, nullptr);
  }
  std::unique_ptr<Buffer> Transport::Gpu::buffer(const void *data, size_t bytes, VkBufferUsageFlags usage) {
    auto result = std::make_unique<Buffer>();
    result->create(physical, device, bytes, usage);
    if (data)
      result->write(data, bytes);
    return result;
  }
  void Transport::initialize(VkPhysicalDevice physical, VkDevice device,
                             std::shared_ptr<const scene::BakeScene> source, Options options) {
    if (!source)
      throw std::invalid_argument("Missing radiance transport scene");
    auto inputs = pack(*source);
    initialize(physical, device, std::move(source), std::move(inputs), options);
  }
  void Transport::initialize(VkPhysicalDevice physical, VkDevice device,
                             std::shared_ptr<const scene::BakeScene> source, Inputs inputs, Options options) {
    if (!source || !source->ready || !std::isfinite(source->sceneScale) || source->sceneScale <= 0 ||
        !options.batchPoints || !options.rayBudget || options.batchPoints > 65535 ||
        options.initialBatchPoints > options.batchPoints || options.textureResolution < 16 ||
        options.textureResolution > 1024 || !std::has_single_bit(options.textureResolution) ||
        !std::isfinite(options.distance) || !std::isfinite(options.bias) || options.bias <= 0 ||
        options.distance <= options.bias * 2 || !std::isfinite(options.distance / source->sceneScale))
      throw std::invalid_argument("Invalid static bake transport options or scene");
    const float distance = options.distance / source->sceneScale, bias = options.bias / source->sceneScale;
    if (!std::isfinite(distance * distance) || !(bias > 0))
      throw std::invalid_argument("Bake units exceed the distance-moment precision range");
    auto next = std::make_unique<Gpu>();
    next->physical = physical;
    next->device = device;
    next->source = std::move(source);
    next->options = options;
    next->timing = std::make_unique<Timing>(physical, device, options.queueFamily);
    next->createInputs(std::move(inputs));
    next->createLayouts();
    next->createPipelines();
    next->createDescriptors();
    m_gpu = std::move(next);
  }
  std::unique_ptr<Buffer> Transport::Gpu::staticBuffer(const void *data, size_t bytes) {
    auto result = std::make_unique<Buffer>();
    result->create(physical, device, bytes,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   Buffer::Memory::Device);
    if (bytes)
      uploads.push_back({buffer(data, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT), result->handle, bytes});
    return result;
  }
  void Transport::Gpu::createInputs(Inputs inputs) {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.limits.maxPerStageDescriptorStorageBuffers < 6 ||
        properties.limits.maxDescriptorSetStorageBuffers < 6)
      throw VK_ERROR_FEATURE_NOT_PRESENT;
    // Storage-buffer range checks happen before GPU allocations.
    const auto limit = properties.limits.maxStorageBufferRange;
    for (auto bytes : {inputs.vertices.size() * sizeof(Vertex), inputs.triangles.size() * sizeof(UInt4),
                       inputs.materials.size() * sizeof(Material), inputs.lights.size() * sizeof(LightData),
                       size_t(options.batchPoints) * sizeof(Result)})
      if (bytes > limit)
        throw VK_ERROR_TOO_MANY_OBJECTS;
    if (inputs.textures.size() + 1 > properties.limits.maxImageArrayLayers)
      throw VK_ERROR_TOO_MANY_OBJECTS;
    resolution = options.textureResolution;
    while (resolution > 16 &&
           uint64_t(resolution) * resolution * 8 * (inputs.textures.size() + 1) > options.atlasBudget)
      resolution /= 2;
    if (uint64_t(resolution) * resolution * 8 * (inputs.textures.size() + 1) > options.atlasBudget)
      throw VK_ERROR_OUT_OF_DEVICE_MEMORY;
    std::vector<RayScene::Position> positions;
    std::vector<uint32_t> indices;
    Bounds sunBounds;
    positions.reserve(inputs.vertices.size());
    indices.reserve(inputs.triangles.size() * 3);
    for (const auto &v : inputs.vertices) {
      positions.push_back({v.position[0], v.position[1], v.position[2]});
      sunBounds.include({v.position[0], v.position[1], v.position[2]});
    }
    double diagonalSquared = 0;
    if (sunBounds.valid)
      for (uint32_t c = 0; c < 3; ++c) {
        const double size = double(sunBounds.maximum[c]) - sunBounds.minimum[c];
        diagonalSquared += size * size;
      }
    sunDistance = std::max(options.distance / source->sceneScale,
                           float(std::sqrt(diagonalSquared) + 2 * options.bias / source->sceneScale));
    if (!std::isfinite(sunDistance))
      throw std::invalid_argument("Sun visibility bounds exceed transport precision");
    for (const auto &t : inputs.triangles)
      indices.insert(indices.end(), t.begin(), t.begin() + 3);
    rays.initialize(physical, device, {positions, indices});
    vertices = staticBuffer(inputs.vertices.data(), inputs.vertices.size() * sizeof(Vertex));
    triangles = staticBuffer(inputs.triangles.data(), inputs.triangles.size() * sizeof(UInt4));
    hasDielectrics = std::any_of(inputs.materials.begin(), inputs.materials.end(), [](const auto &material) {
      return (material.flags[3] & dielectricBoundaryFlag) != 0;
    });
    materials = staticBuffer(inputs.materials.data(), inputs.materials.size() * sizeof(Material));
    lights = staticBuffer(inputs.lights.data(), inputs.lights.size() * sizeof(LightData));
    textures = std::move(inputs.textures);
    domeLayer = inputs.domeLayer;
    for (;;) {
      atlas = std::make_unique<Image>();
      atlas->create(physical, device, {resolution, resolution}, VK_FORMAT_R16G16B16A16_SFLOAT,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(textures.size() + 1), 1,
                    VK_IMAGE_VIEW_TYPE_2D_ARRAY);
      if (atlas->bytes <= options.atlasBudget)
        break;
      if (resolution == 16)
        throw VK_ERROR_OUT_OF_DEVICE_MEMORY;
      resolution /= 2; // Account for device tiling/alignment beyond the nominal texel budget.
    }
    for (uint32_t i = 0; i < textures.size(); ++i) {
      VkImageView view;
      const VkImageViewCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                       .image = atlas->handle,
                                       .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                       .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, i + 1, 1}};
      check(vkCreateImageView(device, &info, nullptr, &view));
      atlasViews.push_back(view);
    }
  }
  void Transport::recordPrepare(VkCommandBuffer command, RasterRenderer &textures) {
    auto &g = *m_gpu;
    if (g.prepared)
      throw std::logic_error("Bake transport is already prepared");
    for (const auto &upload : g.uploads) {
      const VkBufferCopy copy{0, 0, upload.bytes};
      vkCmdCopyBuffer(command, upload.source->handle, upload.target, 1, &copy);
    }
    const VkMemoryBarrier transferred{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                         &transferred, 0, nullptr, 0, nullptr);
    g.recordAtlas(command, textures);
    g.rays.recordBuild(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    g.prepared = true;
  }
  void Transport::recordBatch(VkCommandBuffer command, uint32_t index, std::span<const Point> points,
                              uint32_t samples, uint32_t bounces, uint32_t firstSample) {
    record(command, index, points, samples, bounces, firstSample, Mode::Irradiance);
  }
  void Transport::recordRadiance(VkCommandBuffer command, uint32_t index, std::span<const Point> rays,
                                 uint32_t bounces) {
    record(command, index, rays, 1, bounces, 0, Mode::Radiance);
  }
  void Transport::recordSiteValidation(VkCommandBuffer command, uint32_t slot, std::span<const Point> points,
                                       uint32_t samples) {
    record(command, slot, points, samples, 1, 0, Mode::SiteValidation);
  }
  void Transport::recordVisibility(VkCommandBuffer command, uint32_t slot,
                                   std::span<const VisibilityPoint> points, uint32_t samples,
                                   bool directDiffuse) {
    std::vector<Point> packed;
    packed.reserve(points.size());
    for (const auto &point : points) {
      if (point.light >= m_gpu->source->lights.size() || point.light >= (1u << 24))
        throw std::invalid_argument("Visibility point references an invalid captured light");
      auto surface = point.surface;
      surface.position[3] = float(point.light);
      packed.push_back(surface);
    }
    record(command, slot, packed, samples, 1, 0, directDiffuse ? Mode::DirectVisibility : Mode::Visibility);
  }
  void Transport::recordSun(VkCommandBuffer command, uint32_t slot, std::span<const Point> points,
                            uint32_t samples, bool directDiffuse) {
    if (!m_gpu->source->sun.isDistant)
      throw std::invalid_argument("Sun visibility requires a captured distant light");
    record(command, slot, points, samples, 1, 0,
           directDiffuse ? Mode::SunDirectVisibility : Mode::SunVisibility);
  }
  void Transport::record(VkCommandBuffer command, uint32_t index, std::span<const Point> points,
                         uint32_t samples, uint32_t bounces, uint32_t firstSample, Mode mode) {
    auto &g = *m_gpu;
    if (!g.prepared)
      throw std::logic_error("Bake transport must be prepared before tracing");
    if (points.empty() || points.size() > g.options.batchPoints || samples < 1 || samples > 8192 ||
        bounces < 1 || bounces > 8 || uint64_t(points.size()) * samples * bounces > g.options.rayBudget)
      throw std::invalid_argument("Bake batch exceeds its point, sample or bounce limits");
    for (const auto &point : points) {
      for (auto values : {point.position, point.normal})
        for (float value : values)
          if (!std::isfinite(value))
            throw std::invalid_argument("Non-finite bake point");
      if (mode == Mode::Radiance) {
        const Vec3 direction{point.normal[0], point.normal[1], point.normal[2]};
        const float lengthSquared = dot(direction, direction);
        if (!std::isfinite(lengthSquared) || lengthSquared < 1e-10f)
          throw std::invalid_argument("Invalid reflection capture direction");
      }
    }
    g.reserveSlot(index, uint32_t(points.size()));
    auto &slot = g.slots.at(index);
    slot.points->write(points.data(), points.size_bytes());
    dispatch(command, index, uint32_t(points.size()), samples, bounces, firstSample, mode);
  }
  Transport::DeviceBatch Transport::deviceBatch(uint32_t index) const {
    const auto &g = *m_gpu;
    const auto &slot = g.slots.at(index);
    return {slot.points->descriptor(), slot.results->descriptor(), slot.capacity};
  }
  void Transport::updateLighting(uint32_t index, std::span<const LightData> lights,
                                 const scene::DistantLight &sun, const scene::DomeLight &dome,
                                 float distance) {
    auto &g = *m_gpu;
    if (!std::isfinite(distance) || distance <= g.options.bias * 2 ||
        !std::isfinite(distance / g.source->sceneScale))
      throw std::invalid_argument("Invalid live radiance range");
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(g.physical, &properties);
    if (lights.size_bytes() > properties.limits.maxStorageBufferRange || lights.size() > UINT32_MAX)
      throw VK_ERROR_TOO_MANY_OBJECTS;
    auto &slot = g.slots.at(index);
    if (!slot.liveLights || slot.liveLights->capacity < lights.size_bytes())
      slot.liveLights = g.buffer(nullptr, lights.size_bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    slot.liveLights->write(lights.data(), lights.size_bytes());
    const auto buffer = slot.liveLights->descriptor();
    const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .dstSet = slot.set,
                                     .dstBinding = 5,
                                     .descriptorCount = 1,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                     .pBufferInfo = &buffer};
    vkUpdateDescriptorSets(g.device, 1, &write, 0, nullptr);
    auto frame = lightingFrame(sun, dome, distance, g.options.bias, g.source->sceneScale);
    frame.counts[1] = uint32_t(lights.size());
    slot.lighting = frame;
  }
  VkDescriptorSet Transport::prepareFragment(uint32_t index, uint32_t samples, uint32_t bounces,
                                             float distance) {
    auto &g = *m_gpu;
    if (!g.prepared || samples < 1 || samples > 64 || bounces < 1 || bounces > 8 ||
        !std::isfinite(distance) || distance <= g.options.bias * 2)
      throw std::invalid_argument("Invalid fragment GI transport configuration");
    auto &slot = g.slots.at(index);
    if (!slot.fragmentConstants) {
      slot.fragmentConstants = g.buffer(nullptr, sizeof(Frame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
      const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                 .descriptorPool = g.pool,
                                                 .descriptorSetCount = 1,
                                                 .pSetLayouts = &g.setLayout};
      check(vkAllocateDescriptorSets(g.device, &allocate, &slot.fragment));
    }
    auto frame = slot.lighting.value_or(
        lightingFrame(g.source->sun, g.source->dome, distance, g.options.bias, g.source->sceneScale));
    frame.trace[0] = distance / g.source->sceneScale;
    frame.counts[2] = samples;
    frame.counts[3] = bounces;
    if (!slot.lighting)
      frame.counts[1] = uint32_t(g.source->lights.size());
    frame.control = {g.domeLayer, 0, 1, uint32_t(g.hasDielectrics)};
    slot.fragmentConstants->write(&frame, sizeof(frame));
    const auto acceleration = g.rays.handle();
    const VkWriteDescriptorSetAccelerationStructureKHR rayInfo{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1,
        .pAccelerationStructures = &acceleration};
    const VkDescriptorBufferInfo buffers[] = {
        slot.fragmentConstants->descriptor(), g.vertices->descriptor(), g.triangles->descriptor(),
        g.materials->descriptor(), slot.liveLights ? slot.liveLights->descriptor() : g.lights->descriptor()};
    const VkDescriptorImageInfo atlas{g.sampler, g.atlas->view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[7]{};
    writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .pNext = &rayInfo,
                 .dstSet = slot.fragment,
                 .dstBinding = 0,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    for (uint32_t i = 1; i <= 5; ++i)
      writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                   .dstSet = slot.fragment,
                   .dstBinding = i,
                   .descriptorCount = 1,
                   .descriptorType =
                       i == 1 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                   .pBufferInfo = &buffers[i - 1]};
    writes[6] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = slot.fragment,
                 .dstBinding = 8,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                 .pImageInfo = &atlas};
    vkUpdateDescriptorSets(g.device, 7, writes, 0, nullptr);
    return slot.fragment;
  }
  void Transport::recordDeviceRadiance(VkCommandBuffer command, uint32_t index, uint32_t count,
                                       uint32_t bounces) {
    const auto &g = *m_gpu;
    if (!g.prepared || !count || count > g.slots.at(index).capacity || bounces < 1 || bounces > 8 ||
        uint64_t(count) * bounces > g.options.rayBudget)
      throw std::invalid_argument("GPU radiance batch exceeds its point or bounce limits");
    dispatch(command, index, count, 1, bounces, 0, Mode::Radiance);
  }
  void Transport::dispatch(VkCommandBuffer command, uint32_t index, uint32_t count, uint32_t samples,
                           uint32_t bounces, uint32_t firstSample, Mode mode) {
    auto &g = *m_gpu;
    auto &slot = g.slots.at(index);
    Frame frame;
    if (slot.lighting) {
      frame = *slot.lighting;
    } else {
      frame = lightingFrame(g.source->sun, g.source->dome, g.options.distance, g.options.bias,
                            g.source->sceneScale);
      frame.counts[1] = uint32_t(g.source->lights.size());
    }
    frame.counts[0] = count;
    if (mode == Mode::SunVisibility || mode == Mode::SunDirectVisibility)
      frame.trace[0] = g.sunDistance;
    frame.counts[2] = samples;
    frame.counts[3] = bounces;
    frame.control = {g.domeLayer, firstSample, uint32_t(mode), uint32_t(g.hasDielectrics)};
    slot.constants->write(&frame, sizeof(frame));
    slot.count = count;
    slot.samples = samples;
    g.timing->begin(command, index);
    const bool parallel = g.options.parallel && mode != Mode::Radiance && mode != Mode::SiteValidation;
    slot.compactVisibility = parallel && g.options.cpuReadback &&
                             (mode == Mode::Visibility || mode == Mode::DirectVisibility ||
                              mode == Mode::SunVisibility || mode == Mode::SunDirectVisibility);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                      slot.compactVisibility ? g.visibilityPipeline :
                      parallel ? g.parallelPipeline : g.tracePipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0, 1, &slot.set, 0, nullptr);
    vkCmdDispatch(command, parallel ? slot.count : (slot.count + 63) / 64, 1, 1);
    const VkMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                  .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                  .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
    g.timing->end(command, index);
  }
  bool Transport::timingSupported() const { return m_gpu->timing->supported(); }
  std::optional<double> Transport::elapsed(uint32_t slot) const { return m_gpu->timing->elapsed(slot); }
  void Transport::Gpu::reserveSlot(uint32_t index, uint32_t count) {
    auto &slot = slots.at(index);
    if (count <= slot.capacity)
      return;
    const uint32_t capacity = std::min(options.batchPoints, std::max(count, slot.capacity * 2));
    auto points = buffer(nullptr, size_t(capacity) * sizeof(Point), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto results = std::make_unique<Buffer>();
    results->create(physical, device, size_t(capacity) * sizeof(Result), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    options.cpuReadback ? Buffer::Memory::Readback : Buffer::Memory::Host);
    const VkDescriptorBufferInfo buffers[] = {points->descriptor(), results->descriptor()};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i)
      writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                   .dstSet = slot.set,
                   .dstBinding = 6 + i,
                   .descriptorCount = 1,
                   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                   .pBufferInfo = &buffers[i]};
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    slot.points = std::move(points);
    slot.results = std::move(results);
    slot.capacity = capacity;
  }
  void Transport::read(uint32_t index, std::span<Result> results) const {
    const auto &slot = m_gpu->slots.at(index);
    if (results.size() > slot.count)
      throw std::invalid_argument("Bake readback exceeds the recorded batch");
    if (!slot.compactVisibility) {
      slot.results->read(results.data(), results.size_bytes());
      return;
    }
    // Preserve the general transport readback contract for diagnostics/tests.
    // The controller consumes the compact data directly through readVisibility.
    std::vector<VisibilityResult> compact(results.size());
    readVisibility(index, compact);
    for (size_t i = 0; i < results.size(); ++i) {
      auto &value = results[i];
      value = {};
      value.irradiance[0][0] = compact[i].visibility;
      for (uint32_t coefficient = 0; coefficient < 9; ++coefficient)
        value.irradiance[coefficient][1] = compact[i].irradiance[coefficient];
      value.statistics[2] = float(slot.samples);
    }
  }
  void Transport::readVisibility(uint32_t index, std::span<VisibilityResult> results) const {
    const auto &slot = m_gpu->slots.at(index);
    if (!slot.compactVisibility || results.size() > slot.count)
      throw std::invalid_argument("Readback requires a completed compact visibility batch");
    slot.results->read(results.data(), results.size_bytes());
  }
  void Transport::releaseBuildInputs() {
    m_gpu->rays.releaseBuildInputs();
    m_gpu->uploads.clear();
  }
  uint32_t Transport::textureResolution() const { return m_gpu->resolution; }
  uint64_t Transport::memoryBytes() const {
    const auto &g = *m_gpu;
    uint64_t bytes = g.rays.accelerationBytes() + g.rays.buildBytes() + g.atlas->bytes;
    for (const auto *buffer : {g.vertices.get(), g.triangles.get(), g.materials.get(), g.lights.get()})
      bytes += buffer->allocationBytes;
    for (const auto &slot : g.slots) {
      bytes += slot.constants->allocationBytes + slot.points->allocationBytes + slot.results->allocationBytes;
      if (slot.liveLights)
        bytes += slot.liveLights->allocationBytes;
      if (slot.fragmentConstants)
        bytes += slot.fragmentConstants->allocationBytes;
    }
    for (const auto &upload : g.uploads)
      bytes += upload.source->allocationBytes;
    return bytes;
  }
  uint64_t Transport::imageBytes() const { return m_gpu->atlas->bytes; }
  uint64_t Transport::accelerationBytes() const { return m_gpu->rays.accelerationBytes(); }
} // namespace remix::forward::bake
