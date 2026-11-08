#include "forward/styled_lighting/composition.h"
#include "forward/bake/controller.h"
#include "forward/bake/sparse_sh.h"
#include "forward/styled_lighting/shaders/embedded.h"
#include "platform/gpu_timers.h"
#include "runtime/pipelines.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace remix::forward::styled_lighting {
  struct Resources {
    VkDevice device;
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    std::atomic<uint64_t> buffers{0}, images{0};
    uint64_t bufferBytes() const { return buffers.load(std::memory_order_relaxed); }
    uint64_t imageBytes() const { return images.load(std::memory_order_relaxed); }
    uint64_t allocatedBytes() const { return bufferBytes() + imageBytes(); }
    explicit Resources(VkDevice target) : device(target) {}
    ~Resources() {
      if (pipeline) pipeline::destroyPipeline(device, pipeline, nullptr);
      if (layout) pipeline::destroyPipelineLayout(device, layout, nullptr);
      if (set) pipeline::destroyDescriptorSetLayout(device, set, nullptr);
      if (sampler) vkDestroySampler(device, sampler, nullptr);
    }
    void initialize() {
      VkDescriptorSetLayoutBinding bindings[6];
      for (uint32_t i = 0; i < 6; ++i)
        bindings[i] = {i, i == 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
            i == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
      const VkDescriptorSetLayoutCreateInfo setInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
          .bindingCount = 6, .pBindings = bindings};
      check(pipeline::createDescriptorSetLayout(device, &setInfo, nullptr, &set));
      const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
      const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
          .setLayoutCount = 1, .pSetLayouts = &set, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
      check(pipeline::createPipelineLayout(device, &layoutInfo, nullptr, &layout));
      pipeline::nameCode(composeShader, "forward.stable-styled-lighting");
      const auto module = shaderModule(device, composeShader, sizeof(composeShader));
      const VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = layout};
      const auto result = pipeline::createComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
      pipeline::destroyShaderModule(device, module, nullptr);
      check(result);
      const VkSamplerCreateInfo sampling{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
          .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
          .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
          .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
          .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
          .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
      check(vkCreateSampler(device, &sampling, nullptr, &sampler));
    }
  };
  Publication::~Publication() {
    if (m_completed) vkDestroyEvent(m_resources->device, m_completed, nullptr);
    if (m_pool) vkDestroyDescriptorPool(m_resources->device, m_pool, nullptr);
    if (m_resources) {
      m_resources->buffers.fetch_sub(m_bufferBytes, std::memory_order_relaxed);
      m_resources->images.fetch_sub(m_imageBytes, std::memory_order_relaxed);
    }
  }
  class FieldCache {
  public:
    FieldCache(VkPhysicalDevice physical, std::shared_ptr<Resources> resources,
               uint64_t budget, uint64_t storageRange)
        : m_physical(physical), m_device(resources->device), m_budget(budget),
          m_storageRange(storageRange),
          m_resources(std::move(resources)) {}
    void selectFrame(uint32_t slot);
    std::shared_ptr<const Publication> record(VkCommandBuffer command,
        std::shared_ptr<const bake::Volume> source, const Weights &weights, double seconds, bool useBase);
    uint64_t recordedChunks() const { return m_recordedChunks; }
  private:
    struct Submission { std::shared_ptr<Publication> owner; uint32_t count = 0; };
    std::shared_ptr<Publication> create(std::shared_ptr<const bake::Volume> source, const Weights &weights);
    VkPhysicalDevice m_physical;
    VkDevice m_device;
    uint64_t m_budget, m_storageRange;
    bool m_waiting = false, m_rejected = false;
    uint32_t m_slot = 0;
    double m_changed = 0;
    std::optional<double> m_baseSince;
    Weights m_weights{}, m_readyWeights{};
    std::shared_ptr<const bake::Volume> m_source;
    std::shared_ptr<Resources> m_resources;
    std::shared_ptr<Publication> m_pending, m_ready;
    std::array<Submission, gpuFrameCount> m_submissions;
    uint64_t m_recordedChunks = 0;
  };
  Cache::Cache(VkPhysicalDevice physical, VkDevice device, uint64_t budget)
      : m_physical(physical), m_device(device) {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    m_storageRange = properties.limits.maxStorageBufferRange;
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    uint64_t heap = 0;
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i)
      if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        heap = std::max<uint64_t>(heap, memory.memoryHeaps[i].size);
    m_budget = std::min({budget, maximumBytes, heap / 8});
    VkFormatProperties format;
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R16G16B16A16_SFLOAT, &format);
    const auto required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    m_supported = (format.optimalTilingFeatures & required) == required;
  }
  Cache::~Cache() = default;
  void Cache::prepare() {
    if (!m_supported || m_resources) return;
    auto next = std::make_shared<Resources>(m_device);
    next->initialize();
    m_composed = std::make_unique<FieldCache>(m_physical, next, m_budget, m_storageRange);
    m_resources = std::move(next);
  }
  uint64_t Cache::bufferBytes() const { return m_resources ? m_resources->bufferBytes() : 0; }
  uint64_t Cache::imageBytes() const { return m_resources ? m_resources->imageBytes() : 0; }
  uint64_t Cache::recordedChunks() const {
    return m_composed ? m_composed->recordedChunks() : 0;
  }

  std::shared_ptr<Publication> FieldCache::create(std::shared_ptr<const bake::Volume> source, const Weights &weights) {
    const auto &s = source->compositionLayout;
    const uint64_t records = uint64_t(s.baseRecords) + (uint64_t(s.localWords) + 3) / 4;
    const uint64_t outputBytes = std::max<uint64_t>(records, 1) * 16;
    const uint64_t pixels = uint64_t(s.surfaceSize) * s.surfaceSize * s.surfacePages;
    const VkExtent2D extent{s.surfaceSize, s.surfaceSize};
    const uint32_t layers = s.surfacePages * s.surfaceLayers;
    const uint64_t imageBytes = pixels * s.surfaceLayers * 8;
    constexpr uint64_t parameterBytes = sizeof(Weights) + 32;
    const uint64_t bufferBytes = outputBytes + parameterBytes + 4;
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(m_physical, &properties);
    if (!s.baseRecords || s.baseRecords < 3 + uint64_t(source->irradianceHeader.dimensions[3]) * 16 ||
        !s.surfaceSize || !s.surfacePages || s.surfaceLayers <= lightmap::irradianceLayers ||
        source->data->device != m_device || !source->surfaceAtlas || source->surfaceAtlas->device != m_device ||
        source->data->capacity > m_storageRange || records * 16 > m_storageRange ||
        uint64_t(s.baseRecords) * 16 > source->data->capacity ||
        (s.localWords && uint64_t(s.localFirstRecord) * 16 + uint64_t(s.localWords) * 4 > source->data->capacity) ||
        layers > properties.limits.maxImageArrayLayers || parameterBytes > m_storageRange ||
        records + pixels > UINT32_MAX || imageBytes + bufferBytes > m_budget) {
      m_rejected = true;
      return {};
    }
    // Retired outputs can still belong to submitted consumers. Wait for their
    // fences rather than exceed the cap when a light or source changes.
    const auto fits = [&] { return bufferBytes + imageBytes + m_resources->allocatedBytes() <= m_budget; };
    if (!fits()) {
      m_ready.reset(); // Prefer making progress on the current weights.
      if (!fits()) return {};
    }
    auto next = std::make_shared<Publication>();
    next->m_resources = m_resources;
    next->m_source = std::move(source);
    next->m_surfaceLayers = layers;
    next->m_records = uint32_t(records);
    next->m_pixels = uint32_t(pixels);
    struct Parameters {
      std::array<uint32_t, 4> copy, surface;
      Weights weights;
    } parameters{{s.baseRecords, s.localFirstRecord, s.localWords, uint32_t(records)},
                 {s.surfaceSize, s.surfacePages, s.surfaceLayers, 0}, weights};
    static_assert(sizeof(Parameters) == parameterBytes);
    auto account = [&](const Buffer &buffer) {
      next->m_bufferBytes += buffer.allocationBytes;
      m_resources->buffers.fetch_add(buffer.allocationBytes, std::memory_order_relaxed);
    };
    next->m_parameters.create(m_physical, m_device, parameterBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    account(next->m_parameters);
    next->m_parameters.copyToMapped(&parameters, sizeof(parameters));
    next->m_parameters.flush();
    next->m_data.create(m_physical, m_device, outputBytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Buffer::Memory::Device);
    account(next->m_data);
    next->m_status.create(m_physical, m_device, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Buffer::Memory::Readback);
    account(next->m_status);
    const uint32_t zero = 0;
    next->m_status.write(&zero, 4);
    next->m_surface.create(m_physical, m_device, extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, layers, 1, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    next->m_imageBytes = next->m_surface.bytes;
    m_resources->images.fetch_add(next->m_imageBytes, std::memory_order_relaxed);
    if (m_resources->allocatedBytes() > m_budget) {
      m_ready.reset();
      // Actual alignment can exceed the estimate. If the old result still
      // belongs to a frame, retry after its fence instead of rejecting.
      if (next->m_bufferBytes + next->m_imageBytes > m_budget) m_rejected = true;
      if (m_resources->allocatedBytes() > m_budget) return {};
    }
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = sizes};
    check(vkCreateDescriptorPool(m_device, &pool, nullptr, &next->m_pool));
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = next->m_pool, .descriptorSetCount = 1, .pSetLayouts = &m_resources->set};
    check(vkAllocateDescriptorSets(m_device, &allocate, &next->m_set));
    const VkDescriptorBufferInfo buffers[] = {next->m_source->data->descriptor(), next->m_parameters.descriptor(),
        next->m_data.descriptor(), next->m_status.descriptor()};
    const VkDescriptorImageInfo images[] = {{m_resources->sampler, next->m_source->surfaceAtlas->view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {VK_NULL_HANDLE, next->m_surface.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[6];
    for (uint32_t i = 0; i < 6; ++i) {
      const bool image = i == 3 || i == 4;
      writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = next->m_set,
          .dstBinding = i, .descriptorCount = 1,
          .descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
              i == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pImageInfo = image ? &images[i - 3] : nullptr,
          .pBufferInfo = image ? nullptr : &buffers[i == 5 ? 3 : i]};
    }
    vkUpdateDescriptorSets(m_device, 6, writes, 0, nullptr);
    const VkEventCreateInfo event{.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    check(vkCreateEvent(m_device, &event, nullptr, &next->m_completed));
    return next;
  }
  void Cache::selectFrame(uint32_t slot) {
    if (m_composed) m_composed->selectFrame(slot);
  }
  void FieldCache::selectFrame(uint32_t slot) {
    auto &submission = m_submissions.at(slot);
    if (submission.owner && submission.owner == m_pending) {
      auto &job = *submission.owner;
      const auto status = vkGetEventStatus(m_device, job.m_completed);
      if (status == VK_EVENT_SET) {
        job.m_cursor += submission.count;
        if (job.m_cursor == job.m_records + job.m_pixels) {
          uint32_t invalid = 0;
          job.m_status.read(&invalid, 4);
          if (invalid == 0) {
            m_ready = m_pending;
            m_readyWeights = m_weights;
          } else m_rejected = true;
          m_pending.reset();
        }
      } else if (status != VK_EVENT_RESET) check(status);
      // A reset event after the caller's fence means the recording was
      // abandoned. Retry the same chunk; recording is not completion.
      check(vkResetEvent(m_device, job.m_completed));
      m_waiting = false;
    }
    submission = {};
    m_slot = slot;
  }
  std::shared_ptr<const Publication> Cache::record(VkCommandBuffer command,
      std::shared_ptr<const bake::Volume> source, const Weights &weights, double seconds, bool useBase) {
    return m_composed ? m_composed->record(command, std::move(source), weights, seconds, useBase) : nullptr;
  }
  std::shared_ptr<const Publication> FieldCache::record(VkCommandBuffer command,
      std::shared_ptr<const bake::Volume> source, const Weights &weights, double seconds, bool useBase) {
    const bool sourceChanged = source != m_source;
    if (sourceChanged || std::memcmp(&weights, &m_weights, sizeof(weights)) != 0) {
      if (sourceChanged || (m_ready && m_ready->m_bufferBytes + m_ready->m_imageBytes > m_budget / 2))
        m_ready.reset();
      m_source = std::move(source);
      m_weights = weights;
      m_changed = seconds;
      m_pending.reset();
      m_waiting = m_rejected = false;
    }
    // Keep a completed result briefly across toggles. Expire it even if
    // the first frame after the idle period already uses nonzero weights.
    if (m_baseSince && seconds - *m_baseSince >= 2.0) m_ready.reset();
    if (useBase) {
      if (!m_baseSince) m_baseSince = seconds;
      return {};
    }
    m_baseSince.reset();
    if (!m_source || !m_source->styledLighting || !m_source->data || m_rejected) return {};
    if (m_ready && std::memcmp(&weights, &m_readyWeights, sizeof(weights)) == 0)
      return m_ready;
    if (m_waiting || seconds - m_changed < .1) return {};
    for (const auto &scale : weights)
      for (float value : scale) if (!std::isfinite(value)) { m_rejected = true; return {}; }
    if (!m_pending) {
      try { m_pending = create(m_source, weights); }
      catch (VkResult error) {
        if (error != VK_ERROR_OUT_OF_DEVICE_MEMORY && error != VK_ERROR_OUT_OF_HOST_MEMORY) throw;
        if (m_ready) m_ready.reset();
        else m_rejected = true;
      }
      if (!m_pending) return {};
    }
    auto &job = *m_pending;
    auto &submission = m_submissions[m_slot];
    if (submission.owner) return {};
    const bool surface = job.m_cursor >= job.m_records;
    const uint32_t first = surface ? job.m_cursor - job.m_records : job.m_cursor;
    // One group has a bounded per-element lookup cost. Keep the smaller
    // dispatch when several groups can contribute to the same texel.
    const uint32_t limit = Cache::chunkElements * (m_source->compositionLayout.styleGroups == 1 ? 2u : 1u);
    const uint32_t count = std::min(limit, (surface ? job.m_pixels : job.m_records) - first);
    gpu::GpuScope composition(command, "Forward+ stable styled lighting composition");
    if (job.m_cursor == 0) {
      const VkImageMemoryBarrier start{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = job.m_surface.handle,
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
              job.m_surfaceLayers}};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0, 0, nullptr, 0, nullptr, 1, &start);
    }
    const uint32_t push[] = {uint32_t(surface), first, count};
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, m_resources->pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, m_resources->layout, 0, 1, &job.m_set, 0, nullptr);
    vkCmdPushConstants(command, m_resources->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
    vkCmdDispatch(command, (count + 63) / 64, 1, 1);
    const bool complete = job.m_cursor + count == job.m_records + job.m_pixels;
    const VkMemoryBarrier visible{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT};
    const VkImageMemoryBarrier ready{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = job.m_surface.handle, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
            job.m_surfaceLayers}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &visible, 0, nullptr, complete ? 1u : 0u, complete ? &ready : nullptr);
    vkCmdSetEvent(command, job.m_completed, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    submission = {m_pending, count};
    m_waiting = true;
    ++m_recordedChunks;
    return {}; // Publication waits for this slot's fence and status readback.
  }
}
